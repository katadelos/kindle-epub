package org.katadelos.epub.runtime;

import java.io.*;
import java.util.Properties;
import java.util.zip.CRC32;

/** On-device, Java 1.4-compatible discovery and atomic preparation entry point. */
public final class Main {
    private Main() {
    }

    static void write(File root, String name, byte[] bytes) throws IOException {
        if (name.indexOf("..") >= 0 || name.startsWith("/"))
            throw new IOException("invalid relative artifact name");
        File target = new File(root, name);
        File parent = target.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs())
            throw new IOException("cannot create " + parent);
        FileOutputStream output = new FileOutputStream(target);
        try {
            output.write(bytes);
            output.getFD().sync();
        } finally {
            output.close();
        }
    }

    static void remove(File path) {
        File[] children = path.listFiles();
        if (children != null)
            for (int i = 0; i < children.length; i++)
                remove(children[i]);
        path.delete();
    }

    static boolean className(String value) {
        if (value.length() == 0 || value.length() > 1024)
            return false;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')
                && c != '/' && c != '_' && c != '$')
                return false;
        }
        return value.charAt(0) != '/' && value.charAt(value.length() - 1) != '/' && value.indexOf("//") < 0;
    }

    static void prepare(File destination, String[] sources, PrintStream log) throws IOException {
        destination = destination.getCanonicalFile();
        File parent = destination.getParentFile();
        if (destination.exists() || parent == null || !parent.isDirectory())
            throw new IOException("output must be absent with an existing parent: " + destination);
        Discovery.Match match = new Discovery(log).discover(sources);
        if (!className(match.owner))
            throw new IOException("unsupported class name");
        byte[] adapted = ClassAdapter.transform(match.original, match.getter, match.gate, log);
        // Reparse before publication; callers never observe a partial generation.
        Discovery.ClassFile check = new Discovery.ClassFile(adapted);
        if (!check.name.equals(match.owner))
            throw new IOException("adapted class name changed");
        InputStream stream = Main.class.getResourceAsStream("/" + ClassAdapter.SUPPORT + ".class");
        if (stream == null)
            throw new IOException("missing runtime helper");
        byte[] helper;
        try {
            helper = Discovery.read(stream);
        } finally {
            stream.close();
        }
        File staging = null;
        for (int attempt = 0; attempt < 100; attempt++) {
            File candidate =
                new File(parent, ".epub-java-" + System.currentTimeMillis() + "-" + attempt);
            if (candidate.mkdir()) {
                staging = candidate;
                break;
            }
        }
        if (staging == null)
            throw new IOException("cannot create staging directory");
        try {
            write(staging, "class-name", (match.owner + "\n").getBytes("UTF-8"));
            write(staging, "original.class", match.original);
            write(staging, "replacement.class", adapted);
            write(staging, "shadow/" + match.owner + ".class", adapted);
            write(staging, "shadow/" + ClassAdapter.SUPPORT + ".class", helper);
            write(staging, "helper/" + ClassAdapter.SUPPORT + ".class", helper);
            Properties provenance = new Properties();
            provenance.setProperty("format", "1");
            provenance.setProperty("source.jar", match.jar);
            provenance.setProperty("source.entry", match.owner + ".class");
            provenance.setProperty("source.bytes", String.valueOf(match.original.length));
            CRC32 crc = new CRC32();
            crc.update(match.original);
            provenance.setProperty("source.crc32", Long.toHexString(crc.getValue()));
            provenance.setProperty("getter", match.getter);
            if (match.gate != null)
                provenance.setProperty("suffix.gate", match.gate);
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            provenance.store(buffer, "kindle-epub on-device class adaptation");
            write(staging, "provenance.properties", buffer.toByteArray());
            if (destination.exists() || !staging.renameTo(destination))
                throw new IOException("cannot publish output " + destination);
            staging = null;
            log.println("READY directory=" + destination + " class=" + match.owner);
        } finally {
            if (staging != null)
                remove(staging);
        }
    }

    public static void main(String[] args) {
        if (args.length < 4 || !"prepare".equals(args[0])) {
            System.err.println("Main prepare OUTPUT_DIRECTORY LOG_FILE JAR_OR_DIRECTORY...");
            System.exit(2);
        }
        PrintStream log = null;
        int result = 0;
        try {
            log = new PrintStream(new FileOutputStream(args[2]));
            String[] sources = new String[args.length - 3];
            System.arraycopy(args, 3, sources, 0, sources.length);
            prepare(new File(args[1]), sources, log);
        } catch (Throwable failure) {
            if (log != null) {
                log.println("FAILED " + failure);
                failure.printStackTrace(log);
            }
            result = 1;
        } finally {
            if (log != null)
                log.close();
        }
        if (result != 0)
            System.exit(result);
    }
}
