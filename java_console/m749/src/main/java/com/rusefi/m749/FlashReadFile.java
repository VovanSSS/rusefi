package com.rusefi.m749;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.*;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.Properties;
import static java.nio.file.StandardOpenOption.*;

/** Durable contiguous backup with a local lock and an atomically replaced checkpoint. */
final class FlashReadFile implements AutoCloseable {
    final int address, length;
    private final Path output, partial, checkpoint;
    private FileChannel lockChannel, data;
    private FileLock lock;
    private final Properties state = new Properties();
    private MessageDigest digest = digest();
    private int completed;

    FlashReadFile(Path output, int address, int length, boolean resume) throws IOException {
        M749RamHelper.requireRange(address, length);
        this.address = address;
        this.length = length;
        Path absolute = output.toAbsolutePath().normalize();
        if (!Files.isDirectory(absolute.getParent())) { throw new IOException("Output directory does not exist"); }
        this.output = absolute.getParent().toRealPath().resolve(absolute.getFileName());
        partial = this.output.resolveSibling(this.output.getFileName() + ".part");
        checkpoint = this.output.resolveSibling(this.output.getFileName() + ".properties");
        try {
            // Keep advisory locking on a native local filesystem, including when
            // the backup itself is on a Windows UNC path.
            String key = this.output.toString();
            if (System.getProperty("os.name").toLowerCase(Locale.ROOT).startsWith("windows")) { key = key.toLowerCase(Locale.ROOT); }
            Path lockPath = Path.of(System.getProperty("java.io.tmpdir"), "m749-read-" + hash(key.getBytes(java.nio.charset.StandardCharsets.UTF_8)) + ".lock");
            lockChannel = FileChannel.open(lockPath, CREATE, WRITE);
            try { lock = lockChannel.tryLock(); }
            catch (OverlappingFileLockException e) { throw new IOException("This backup is already open", e); }
            if (lock == null) { throw new IOException("This backup is already open in another process"); }
            if (Files.exists(this.output)) { throw new IOException("Output already exists; refusing to overwrite " + this.output); }
            if (resume) {
                try (InputStream in = Files.newInputStream(checkpoint)) { state.load(in); }
                require("format", "M749FLASH1");
                require("helper.sha256", M749RamHelper.SHA256);
                require("address", Integer.toString(address));
                require("length", Integer.toString(length));
                try { completed = Integer.parseInt(state.getProperty("completed", "-1")); }
                catch (NumberFormatException e) { throw new IOException("Invalid saved progress", e); }
                if (completed < 0 || completed > length || Files.isSymbolicLink(partial)) { throw new IOException("Invalid saved progress/file"); }
                data = FileChannel.open(partial, READ, WRITE);
                if (data.size() < completed || data.size() > length) { throw new IOException("Partial file size does not match saved progress"); }
                for (int position = 0; position < completed; position += 65536) {
                    digest.update(saved(position, Math.min(65536, completed - position)));
                }
                require("sha256", currentHash());
                // Bytes after the last durable checkpoint have not been certified.
                data.truncate(completed);
            } else {
                if (Files.exists(partial) || Files.exists(checkpoint)) { throw new IOException("Partial backup exists; use --resume or a new output filename"); }
                data = FileChannel.open(partial, CREATE_NEW, READ, WRITE);
                state.setProperty("format", "M749FLASH1");
                state.setProperty("helper.sha256", M749RamHelper.SHA256);
                state.setProperty("address", Integer.toString(address));
                state.setProperty("length", Integer.toString(length));
                state.setProperty("verification", "two-identical-reads-per-block");
                state.setProperty("created", java.time.Instant.now().toString());
                data.force(true);
                save(false);
            }
        } catch (IOException | RuntimeException e) {
            try { close(); } catch (IOException close) { e.addSuppressed(close); }
            throw e;
        }
    }

    int completed() { return completed; }

    void identify(String identity) throws IOException {
        String saved = state.getProperty("cpu.identification");
        if (saved != null && !saved.equals(identity)) { throw new IOException("Helper CPU identification differs from this backup"); }
        state.setProperty("cpu.identification", identity);
        save(false);
    }

    byte[] saved(int position, int count) throws IOException {
        if (position < 0 || count < 0 || (long) position + count > completed) { throw new IOException("Invalid saved range"); }
        ByteBuffer buffer = ByteBuffer.allocate(count);
        while (buffer.hasRemaining()) {
            int read = data.read(buffer, position + buffer.position());
            if (read <= 0) { throw new IOException("Cannot read saved backup bytes"); }
        }
        return buffer.array();
    }

    void append(byte[] bytes) throws IOException {
        if (bytes.length == 0 || (long) completed + bytes.length > length) { throw new IOException("Invalid backup block"); }
        ByteBuffer buffer = ByteBuffer.wrap(bytes);
        while (buffer.hasRemaining()) {
            if (data.write(buffer, completed + buffer.position()) <= 0) { throw new IOException("Cannot write backup bytes"); }
        }
        data.force(true);
        digest.update(bytes);
        completed += bytes.length;
        save(false);
    }

    String finish() throws IOException {
        if (completed != length) { throw new IOException("Backup coverage is incomplete"); }
        MessageDigest actual = digest();
        for (int position = 0; position < length; position += 65536) {
            actual.update(saved(position, Math.min(65536, length - position)));
        }
        String checksum = hex(actual.digest());
        if (!checksum.equals(currentHash())) { throw new IOException("Saved backup hash differs from received data"); }
        data.force(true);
        save(true);
        data.close();
        data = null;
        // No REPLACE_EXISTING: never destroy a concurrently created final file.
        Files.move(partial, output);
        return checksum;
    }

    private void require(String key, String value) throws IOException {
        if (!value.equals(state.getProperty(key))) { throw new IOException("Checkpoint mismatch: " + key); }
    }

    private void save(boolean complete) throws IOException {
        state.setProperty("completed", Integer.toString(completed));
        state.setProperty("sha256", currentHash());
        state.setProperty("complete", Boolean.toString(complete));
        Path temp = Files.createTempFile(checkpoint.getParent(), ".m749-read-", ".tmp");
        try {
            try (java.io.OutputStream out = Files.newOutputStream(temp)) { state.store(out, "M74.9 main-flash backup"); }
            try (FileChannel file = FileChannel.open(temp, WRITE)) { file.force(true); }
            for (int attempt = 0; ; attempt++) {
                try {
                    Files.move(temp, checkpoint, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                    break;
                } catch (AccessDeniedException e) {
                    if (attempt == 4) { throw e; }
                    try { Thread.sleep(50); }
                    catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new IOException("Checkpoint interrupted", interrupted); }
                }
            }
        } finally { Files.deleteIfExists(temp); }
    }

    private String currentHash() {
        try { return hex(((MessageDigest) digest.clone()).digest()); }
        catch (CloneNotSupportedException e) { throw new IllegalStateException("SHA-256 provider must support snapshots", e); }
    }

    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }

    static String hash(byte[] bytes) { return hex(digest().digest(bytes)); }
    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte value : bytes) { text.append(String.format("%02x", value & 255)); }
        return text.toString();
    }

    public void close() throws IOException {
        try { if (data != null) { data.close(); } }
        finally {
            try { if (lock != null && lock.isValid()) { lock.release(); } }
            finally { if (lockChannel != null) { lockChannel.close(); } }
        }
    }
}
