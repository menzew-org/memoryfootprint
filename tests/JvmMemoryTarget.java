public final class JvmMemoryTarget {
    private static volatile long checksum;

    private JvmMemoryTarget() {}

    public static void main(String[] args) throws Exception {
        byte[][] blocks = new byte[16][];
        for (int index = 0; index < blocks.length; ++index) {
            blocks[index] = new byte[1024 * 1024];
            for (int offset = 0; offset < blocks[index].length; offset += 4096) {
                blocks[index][offset] = (byte) (index + 1);
                checksum += blocks[index][offset];
            }
        }
        Thread.sleep(15000);
        if (checksum == 0) {
            throw new AssertionError("memory was not touched");
        }
    }
}
