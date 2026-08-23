/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small Java 8-compatible helpers shared by the JVM footprint workloads.
 */

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;

final class StudySupport {
    private StudySupport() {}

    static void writePidFile() throws Exception {
        String path = System.getProperty("memftprt.study.pidFile");
        if (path == null || path.isEmpty()) {
            throw new IllegalArgumentException("memftprt.study.pidFile is required");
        }

        String runtimeName = ManagementFactory.getRuntimeMXBean().getName();
        int separator = runtimeName.indexOf('@');
        String pid = separator < 0 ? runtimeName : runtimeName.substring(0, separator);
        File target = new File(path);
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IllegalStateException("could not create PID-file directory: " + parent);
        }

        Writer writer = new OutputStreamWriter(new FileOutputStream(target),
                                                StandardCharsets.UTF_8);
        try {
            writer.write(pid);
            writer.write('\n');
        } finally {
            writer.close();
        }
    }

    static void ready(String workload, String details) {
        System.out.println("MEMFTPRT_STUDY_READY workload=" + workload + " " + details);
        System.out.flush();
    }

    static void parkForever() throws InterruptedException {
        Object monitor = new Object();
        synchronized (monitor) {
            for (;;) {
                monitor.wait();
            }
        }
    }
}
