/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Java 8-compatible sustained workload for comparing JVM runtime footprints.
 * It exercises allocation, GCs, tiered compilation, synchronization, class
 * metadata, reflection, proxies, and worker stacks, then parks without asking
 * for a collection so memftprt can capture the naturally retained JVM state.
 */

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;

public final class RuntimeExercise {
    private static final int WORKERS = 4;
    private static final int RETAINED_SLOTS = 256;
    private static final byte[][] RETAINED = new byte[RETAINED_SLOTS][];
    private static final Object RETAINED_LOCK = new Object();
    private static final AtomicLong OPERATIONS = new AtomicLong();
    private static volatile long checksum;

    private RuntimeExercise() {}

    private interface Calculator {
        long apply(long value);
    }

    private static long hotMethod(long value) {
        long mixed = value;
        for (int index = 0; index < 32; ++index) {
            mixed ^= mixed << 13;
            mixed ^= mixed >>> 7;
            mixed ^= mixed << 17;
            mixed += index * 0x9e3779b97f4a7c15L;
        }
        return mixed;
    }

    private static Calculator createProxy() {
        InvocationHandler handler = new InvocationHandler() {
            @Override
            public Object invoke(Object proxy, Method method, Object[] args) {
                return Long.valueOf(hotMethod(((Long) args[0]).longValue()));
            }
        };
        return (Calculator) Proxy.newProxyInstance(RuntimeExercise.class.getClassLoader(),
                                                    new Class<?>[] {Calculator.class}, handler);
    }

    private static void runWorker(int worker, long deadline, CountDownLatch completed,
                                  Calculator calculator) {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (Exception error) {
            throw new RuntimeException(error);
        }

        long local = worker + 1L;
        long iteration = 0;
        while (System.nanoTime() < deadline) {
            byte[] allocation = new byte[16 * 1024 + (int) (iteration & 4095)];
            Arrays.fill(allocation, (byte) local);
            digest.update(allocation, 0, 256);

            List<Long> values = new ArrayList<Long>(32);
            Map<Long, Long> lookup = new HashMap<Long, Long>();
            for (int index = 0; index < 32; ++index) {
                local = hotMethod(local + index);
                Long boxed = Long.valueOf(local);
                values.add(boxed);
                lookup.put(boxed, Long.valueOf(index));
            }
            local ^= values.get((int) (iteration & 31)).longValue();
            local ^= lookup.size();

            if ((iteration & 255) == 0) {
                local ^= calculator.apply(local);
                byte[] retained = new byte[64 * 1024];
                retained[0] = (byte) local;
                retained[retained.length - 1] = (byte) (local >>> 8);
                synchronized (RETAINED_LOCK) {
                    RETAINED[(int) ((iteration / 256 + worker * 61) % RETAINED.length)] =
                        retained;
                }
            }

            ++iteration;
            if ((iteration & 1023) == 0) {
                OPERATIONS.addAndGet(1024);
            }
        }

        OPERATIONS.addAndGet(iteration & 1023);
        byte[] finalDigest = digest.digest();
        for (byte value : finalDigest) {
            local = local * 31 + value;
        }
        checksum ^= local;
        completed.countDown();

        // Preserve the exercised worker stacks in a known idle state.
        for (;;) {
            LockSupport.park();
        }
    }

    public static void main(String[] args) throws Exception {
        int durationSeconds = args.length == 0 ? 150 : Integer.parseInt(args[0]);
        if (durationSeconds < 1 || durationSeconds > 900) {
            throw new IllegalArgumentException("duration must be between 1 and 900 seconds");
        }

        StudySupport.writePidFile();
        final Calculator calculator = createProxy();
        final CountDownLatch completed = new CountDownLatch(WORKERS);
        final long started = System.nanoTime();
        final long deadline = started + durationSeconds * 1_000_000_000L;

        for (int worker = 0; worker < WORKERS; ++worker) {
            final int workerId = worker;
            Thread thread = new Thread(new Runnable() {
                @Override
                public void run() {
                    runWorker(workerId, deadline, completed, calculator);
                }
            }, "footprint-worker-" + worker);
            thread.setDaemon(true);
            thread.start();
        }

        completed.await();
        long elapsedMillis = (System.nanoTime() - started) / 1_000_000L;
        StudySupport.ready("exercise",
            "duration_ms=" + elapsedMillis +
            " operations=" + OPERATIONS.get() +
            " workers=" + WORKERS +
            " checksum=" + checksum);
        StudySupport.parkForever();
    }
}
