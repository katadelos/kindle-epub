package org.katadelos.epub.runtime;

import java.io.*;
import java.util.*;

/** Adds small wrappers while retaining the complete original method bodies. */
final class ClassAdapter {
    static final String SUPPORT = "org/katadelos/epub/EpubSupport";
    static final class Pool {
        byte[] original;
        int count;
        Discovery.ClassFile source;
        ByteArrayOutputStream added = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(added);
        int utf(String s) throws IOException {
            if (count >= 65535)
                throw new IOException("constant pool full");
            int n = count++;
            out.writeByte(1);
            out.writeUTF(s);
            return n;
        }
        int cls(String s) throws IOException {
            int name = utf(s), n = count++;
            out.writeByte(7);
            out.writeShort(name);
            return n;
        }
        int method(String owner, String name, String desc) throws IOException {
            int type = cls(owner), nn = utf(name), dd = utf(desc), nt = count++;
            out.writeByte(12);
            out.writeShort(nn);
            out.writeShort(dd);
            int n = count++;
            out.writeByte(10);
            out.writeShort(type);
            out.writeShort(nt);
            return n;
        }
    }
    static final class Member {
        int access, name, desc;
        ArrayList attrs = new ArrayList();
        void write(DataOutputStream out) throws IOException {
            out.writeShort(access);
            out.writeShort(name);
            out.writeShort(desc);
            out.writeShort(attrs.size());
            for (int i = 0; i < attrs.size(); i++)
                out.write((byte[]) attrs.get(i));
        }
    }
    static int skipPool(DataInputStream in) throws IOException {
        int count = in.readUnsignedShort();
        for (int i = 1; i < count; i++) {
            int tag = in.readUnsignedByte();
            switch (tag) {
                case 1:
                    in.readUTF();
                    break;
                case 3:
                case 4:
                    in.readInt();
                    break;
                case 5:
                case 6:
                    in.readLong();
                    i++;
                    break;
                case 7:
                case 8:
                case 16:
                case 19:
                case 20:
                    in.readUnsignedShort();
                    break;
                case 9:
                case 10:
                case 11:
                case 12:
                case 17:
                case 18:
                    in.readInt();
                    break;
                case 15:
                    in.readUnsignedByte();
                    in.readUnsignedShort();
                    break;
                default:
                    throw new IOException("Unknown constant tag " + tag);
            }
        }
        return count;
    }
    static byte[] attr(DataInputStream in) throws IOException {
        int name = in.readUnsignedShort();
        byte[] data = Discovery.block(in);
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        DataOutputStream o = new DataOutputStream(b);
        o.writeShort(name);
        o.writeInt(data.length);
        o.write(data);
        return b.toByteArray();
    }
    static Member member(DataInputStream in) throws IOException {
        Member m = new Member();
        m.access = in.readUnsignedShort();
        m.name = in.readUnsignedShort();
        m.desc = in.readUnsignedShort();
        int n = in.readUnsignedShort();
        for (int i = 0; i < n; i++)
            m.attrs.add(attr(in));
        return m;
    }
    static void invoke(DataOutputStream code, int op, int index) throws IOException {
        code.writeByte(op);
        code.writeShort(index);
    }
    static Member wrapper(Member original, String name, String desc, String owner, Pool pool,
        boolean gate) throws IOException {
        if ((original.access & (0x100 | 0x400)) != 0)
            throw new IOException("Cannot wrap native/abstract method");
        Member m = new Member();
        m.access = original.access;
        m.name = original.name;
        m.desc = original.desc;
        // Keep method metadata (annotations, exceptions, signature) on the
        // public wrapper. Only its Code attribute is replaced.
        for (int i = 0; i < original.attrs.size(); i++) {
            byte[] attr = (byte[]) original.attrs.get(i);
            int index = ((attr[0] & 255) << 8) | (attr[1] & 255);
            if (!"Code".equals(pool.source.utf(index)))
                m.attrs.add(attr);
        }
        String renamed = "epub$original$" + name;
        original.name = pool.utf(renamed);
        original.access = (original.access & ~(1 | 4 | 32)) | 2;
        boolean stat = (m.access & 8) != 0;
        int originalRef = pool.method(owner, renamed, desc);
        String supportDesc = "(ZLjava/lang/String;)Z";
        if (!gate) {
            String result = desc.substring(2);
            supportDesc = "(" + result + ")" + result;
        }
        int supportRef = pool.method(SUPPORT, gate ? "accept" : "extensions", supportDesc);
        ByteArrayOutputStream instructions = new ByteArrayOutputStream();
        DataOutputStream code = new DataOutputStream(instructions);
        if (!stat)
            code.writeByte(42);
        if (gate)
            code.writeByte(stat ? 42 : 43);
        invoke(code, stat ? 184 : 183, originalRef);
        if (gate)
            code.writeByte(stat ? 42 : 43);
        invoke(code, 184, supportRef);
        code.writeByte(gate ? 172 : 176);
        ByteArrayOutputStream content = new ByteArrayOutputStream();
        DataOutputStream body = new DataOutputStream(content);
        body.writeShort(3);
        body.writeShort(gate ? (stat ? 1 : 2) : (stat ? 0 : 1));
        body.writeInt(instructions.size());
        body.write(instructions.toByteArray());
        body.writeShort(0);
        body.writeShort(0);
        ByteArrayOutputStream attribute = new ByteArrayOutputStream();
        DataOutputStream a = new DataOutputStream(attribute);
        a.writeShort(pool.utf("Code"));
        a.writeInt(content.size());
        a.write(content.toByteArray());
        m.attrs.add(attribute.toByteArray());
        return m;
    }
    static byte[] transform(byte[] bytes, String getter, String gate, PrintStream log)
        throws IOException {
        Discovery.ClassFile parsed = new Discovery.ClassFile(bytes);
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(bytes));
        in.readLong();
        Pool pool = new Pool();
        pool.count = skipPool(in);
        pool.source = parsed;
        int end = bytes.length - in.available();
        pool.original = new byte[end - 10];
        System.arraycopy(bytes, 10, pool.original, 0, pool.original.length);
        int access = in.readUnsignedShort(), thisClass = in.readUnsignedShort(),
            superClass = in.readUnsignedShort();
        if ((access & 0x200) != 0)
            throw new IOException("interface adaptation is unsupported");
        int ni = in.readUnsignedShort();
        int[] interfaces = new int[ni];
        for (int i = 0; i < ni; i++)
            interfaces[i] = in.readUnsignedShort();
        int nf = in.readUnsignedShort();
        ArrayList fields = new ArrayList();
        for (int i = 0; i < nf; i++)
            fields.add(member(in));
        int nm = in.readUnsignedShort();
        ArrayList methods = new ArrayList(), added = new ArrayList();
        int found = 0;
        for (int i = 0; i < nm; i++) {
            Member m = member(in);
            String name = parsed.utf(m.name), desc = parsed.utf(m.desc), signature = name + desc;
            if (name.startsWith("epub$original$"))
                throw new IOException("Input already adapted");
            if (signature.equals(getter) || (gate != null && signature.equals(gate))) {
                boolean g = signature.equals(gate);
                added.add(wrapper(m, name, desc, parsed.name, pool, g));
                found++;
                log.println("WRAP class=" + parsed.name + " method=" + signature
                    + " kind=" + (g ? "stock-or-epub" : "append-extension"));
            }
            methods.add(m);
        }
        if (found != (gate == null ? 1 : 2))
            throw new IOException("Discovered method cardinality changed: " + found);
        int na = in.readUnsignedShort();
        ArrayList attrs = new ArrayList();
        for (int i = 0; i < na; i++)
            attrs.add(attr(in));
        if (in.available() != 0)
            throw new IOException("Trailing class data");
        methods.addAll(added);
        if (pool.count > 65535 || methods.size() > 65535)
            throw new IOException("class table overflow");
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(result);
        out.write(bytes, 0, 8);
        out.writeShort(pool.count);
        out.write(pool.original);
        out.write(pool.added.toByteArray());
        out.writeShort(access);
        out.writeShort(thisClass);
        out.writeShort(superClass);
        out.writeShort(ni);
        for (int i = 0; i < ni; i++)
            out.writeShort(interfaces[i]);
        out.writeShort(fields.size());
        for (int i = 0; i < fields.size(); i++)
            ((Member) fields.get(i)).write(out);
        out.writeShort(methods.size());
        for (int i = 0; i < methods.size(); i++)
            ((Member) methods.get(i)).write(out);
        out.writeShort(attrs.size());
        for (int i = 0; i < attrs.size(); i++)
            out.write((byte[]) attrs.get(i));
        return result.toByteArray();
    }
}
