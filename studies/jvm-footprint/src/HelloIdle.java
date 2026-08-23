/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal live JVM workload. It deliberately stays alive after printing so
 * memftprt can observe a settled process instead of racing JVM shutdown.
 */

public final class HelloIdle {
    private HelloIdle() {}

    public static void main(String[] args) throws Exception {
        StudySupport.writePidFile();
        System.out.println("Hello, world!");
        StudySupport.ready("hello", "message=printed");
        StudySupport.parkForever();
    }
}
