package org.katadelos.epub.runtime;

import java.io.*;
import java.util.*;
import java.util.zip.*;

/** Bounded structural discovery. Parsing never loads or initializes firmware classes. */
final class Discovery {
    static final int MAX_CLASS_BYTES = 2 * 1024 * 1024;
    static final int MAX_CLASSES = 200000;
    static final int MAX_JARS = 4096;
    static final int MAX_CANDIDATES = 64;
    long bytesRead;
    static final class Match {
        final String jar, owner, getter, gate;
        final byte[] original;
        Match(String j, ClassFile c, String g, String s) {
            jar = j;
            owner = c.name;
            getter = g;
            gate = s;
            original = c.original;
        }
    }

    final PrintStream out;
    Discovery(PrintStream log) {
        out = log;
    }
    int jars, classes, candidates;
    final ArrayList resolved = new ArrayList();
    final HashSet visited = new HashSet();
    static final String STRINGS = "[Ljava/lang/String;";
    static final Object CATEGORY2 = new Object();
    static final Object THIS = new Object();
    static final Object OTHER_LOCAL = new Object();

    static final class Ref {
        String owner, name, desc;
        Ref(String o, String n, String d) {
            owner = o;
            name = n;
            desc = d;
        }
        public String toString() {
            return owner + "." + name + ":" + desc;
        }
    }
    static final class Method {
        int access;
        String name, desc;
        byte[] code;
        Method(int a, String n, String d) {
            access = a;
            name = n;
            desc = d;
        }
        public String toString() {
            return name + desc;
        }
    }
    static final class ArrayValue {
        String[] values;
        ArrayValue(int size) {
            values = new String[size];
        }
        boolean target() {
            boolean a = false, k = false;
            for (int i = 0; i < values.length; i++) {
                if (values[i] == null)
                    return false;
                a |= "azw3".equals(values[i]);
                k |= "kdl".equals(values[i]);
            }
            return a && k;
        }
        public String toString() {
            StringBuffer b = new StringBuffer("[");
            for (int i = 0; i < values.length; i++) {
                if (i > 0)
                    b.append(",");
                b.append(values[i]);
            }
            return b.append("]").toString();
        }
    }
    static final class ClassFile {
        int[] tags;
        Object[] cp;
        String name, superName;
        ArrayList methods = new ArrayList();
        ArrayList fields = new ArrayList();
        HashMap instanceArrays = new HashMap();
        HashMap instanceWrites = new HashMap();
        ArrayList listGetters = new ArrayList();
        ArrayList suffixGates = new ArrayList();
        String listGetField, listSizeField;
        final byte[] original;
        int major;
        ClassFile(byte[] data) throws IOException {
            if (data.length > MAX_CLASS_BYTES)
                throw new IOException("class exceeds size bound");
            original = data;
            DataInputStream in = new DataInputStream(new ByteArrayInputStream(data));
            if (in.readInt() != 0xcafebabe)
                throw new IOException("bad class magic");
            in.readUnsignedShort();
            major = in.readUnsignedShort();
            int count = in.readUnsignedShort();
            if (count == 0)
                throw new IOException("empty constant pool");
            tags = new int[count];
            cp = new Object[count];
            for (int i = 1; i < count; i++) {
                int tag = in.readUnsignedByte();
                tags[i] = tag;
                switch (tag) {
                    case 1:
                        cp[i] = in.readUTF();
                        break;
                    case 3:
                        cp[i] = new Integer(in.readInt());
                        break;
                    case 4:
                        in.readInt();
                        break;
                    case 5:
                    case 6:
                        in.readLong();
                        if (++i >= count)
                            throw new IOException("truncated wide constant");
                        break;
                    case 7:
                    case 8:
                    case 16:
                    case 19:
                    case 20:
                        cp[i] = new Integer(in.readUnsignedShort());
                        break;
                    case 9:
                    case 10:
                    case 11:
                    case 12:
                    case 17:
                    case 18:
                        cp[i] = new int[] {in.readUnsignedShort(), in.readUnsignedShort()};
                        break;
                    case 15:
                        cp[i] = new int[] {in.readUnsignedByte(), in.readUnsignedShort()};
                        break;
                    default:
                        throw new IOException("unknown CP tag " + tag);
                }
            }
            in.readUnsignedShort();
            name = className(in.readUnsignedShort());
            int s = in.readUnsignedShort();
            superName = s == 0 ? "" : className(s);
            int ni = in.readUnsignedShort();
            for (int i = 0; i < ni; i++)
                in.readUnsignedShort();
            int nf = in.readUnsignedShort();
            for (int i = 0; i < nf; i++) {
                int access = in.readUnsignedShort();
                String n = utf(in.readUnsignedShort()), d = utf(in.readUnsignedShort());
                fields.add(new Object[] {new Integer(access), new Ref(name, n, d)});
                skipAttributes(in);
            }
            int nm = in.readUnsignedShort();
            for (int i = 0; i < nm; i++) {
                Method m = new Method(in.readUnsignedShort(), utf(in.readUnsignedShort()),
                    utf(in.readUnsignedShort()));
                int na = in.readUnsignedShort();
                for (int a = 0; a < na; a++) {
                    String an = utf(in.readUnsignedShort());
                    byte[] bytes = block(in);
                    if ("Code".equals(an)) {
                        if (m.code != null)
                            throw new IOException("duplicate Code attribute");
                        DataInputStream c = new DataInputStream(new ByteArrayInputStream(bytes));
                        c.readUnsignedShort();
                        c.readUnsignedShort();
                        m.code = block(c);
                        if (m.code.length == 0 || m.code.length > 65535)
                            throw new IOException("invalid Code length");
                        int ne = c.readUnsignedShort();
                        for (int e = 0; e < ne; e++) {
                            c.readUnsignedShort();
                            c.readUnsignedShort();
                            c.readUnsignedShort();
                            c.readUnsignedShort();
                        }
                        skipAttributes(c);
                        if (c.available() != 0)
                            throw new IOException("trailing Code bytes");
                    }
                }
                methods.add(m);
            }
            skipAttributes(in);
            if (in.available() != 0)
                throw new IOException("trailing class bytes");
        }
        void skipAttributes(DataInputStream in) throws IOException {
            int count = in.readUnsignedShort();
            for (int i = 0; i < count; i++) {
                utf(in.readUnsignedShort());
                block(in);
            }
        }
        String utf(int n) throws IOException {
            if (n <= 0 || n >= tags.length || tags[n] != 1)
                throw new IOException("bad UTF reference " + n);
            return (String) cp[n];
        }
        String className(int n) throws IOException {
            if (n <= 0 || n >= tags.length || tags[n] != 7)
                throw new IOException("bad class reference " + n);
            return utf(((Integer) cp[n]).intValue());
        }
        Object constant(int n) throws IOException {
            if (n <= 0 || n >= tags.length)
                throw new IOException("bad constant " + n);
            if (tags[n] == 8)
                return utf(((Integer) cp[n]).intValue());
            if (tags[n] == 3)
                return cp[n];
            if (tags[n] == 5 || tags[n] == 6)
                return CATEGORY2;
            return null;
        }
        Ref ref(int n) throws IOException {
            if (n <= 0 || n >= tags.length || (tags[n] != 9 && tags[n] != 10 && tags[n] != 11))
                throw new IOException("bad member reference " + n);
            int[] r = (int[]) cp[n];
            if (r[1] <= 0 || r[1] >= tags.length || tags[r[1]] != 12)
                throw new IOException("bad name/type");
            int[] nt = (int[]) cp[r[1]];
            return new Ref(className(r[0]), utf(nt[0]), utf(nt[1]));
        }
        boolean shortlist() throws IOException {
            boolean a = false, k = false;
            for (int i = 1; i < tags.length; i++)
                if (tags[i] == 8) {
                    String s = (String) constant(i);
                    a |= extension(s, "azw3");
                    k |= extension(s, "kdl");
                }
            return a && k;
        }
    }

    static byte[] block(DataInputStream in) throws IOException {
        int n = in.readInt();
        if (n < 0 || n > MAX_CLASS_BYTES || n > in.available())
            throw new IOException("oversized block " + n);
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }
    static boolean extension(String value, String name) {
        return name.equals(value) || ("." + name).equals(value);
    }
    static int u1(byte[] b, int p) throws IOException {
        if (p < 0 || p >= b.length)
            throw new IOException("truncated code");
        return b[p] & 255;
    }
    static int u2(byte[] b, int p) throws IOException {
        return (u1(b, p) << 8) | u1(b, p + 1);
    }
    static int i4(byte[] b, int p) throws IOException {
        return (u1(b, p) << 24) | (u1(b, p + 1) << 16) | (u1(b, p + 2) << 8) | u1(b, p + 3);
    }
    static int next(byte[] b, int p) throws IOException {
        int op = u1(b, p), n = 1;
        if (op == 170 || op == 171) {
            int q = (p + 4) & ~3;
            i4(b, q);
            long end;
            if (op == 170) {
                int low = i4(b, q + 4), high = i4(b, q + 8);
                long count = (long) high - low + 1;
                if (count < 0)
                    throw new IOException("bad tableswitch");
                end = q + 12L + 4L * count;
            } else {
                int count = i4(b, q + 4);
                if (count < 0)
                    throw new IOException("bad lookupswitch");
                end = q + 8L + 8L * count;
            }
            if (end > b.length)
                throw new IOException("truncated switch");
            return (int) end;
        }
        if (op == 196) {
            int nested = u1(b, p + 1);
            n = nested == 132 ? 6 : 4;
            if (nested != 132 && nested != 169 && !(nested >= 21 && nested <= 25)
                && !(nested >= 54 && nested <= 58))
                throw new IOException("bad wide");
        } else if (op == 16 || op == 18 || (op >= 21 && op <= 25) || (op >= 54 && op <= 58)
            || op == 169 || op == 188)
            n = 2;
        else if (op == 17 || op == 19 || op == 20 || op == 132 || (op >= 153 && op <= 168)
            || (op >= 178 && op <= 184) || op == 187 || op == 189 || op == 192 || op == 193
            || op == 198 || op == 199)
            n = 3;
        else if (op == 185 || op == 186 || op == 200 || op == 201)
            n = 5;
        else if (op == 197)
            n = 4;
        else if (op > 201)
            throw new IOException("reserved opcode " + op);
        if (p + n > b.length)
            throw new IOException("truncated instruction");
        return p + n;
    }
    static boolean arrayOrList(String d) {
        return STRINGS.equals(d) || "Ljava/util/List;".equals(d) || "Ljava/util/Vector;".equals(d)
            || "Ljava/util/ArrayList;".equals(d);
    }
    static boolean getter(Method m) {
        return ("()" + STRINGS).equals(m.desc) || "()Ljava/util/List;".equals(m.desc);
    }
    static Object pop(ArrayList stack) {
        return stack.isEmpty() ? null : stack.remove(stack.size() - 1);
    }
    static void discard(ArrayList stack, int slots) {
        // The symbolic evaluator only reasons about category-1 values. Unknown or wide
        // values terminate the little symbolic segment instead of guessing.
        if (stack.size() < slots) {
            stack.clear();
            return;
        }
        for (int i = 0; i < slots; i++) {
            Object v = pop(stack);
            if (v == null || v == CATEGORY2) {
                stack.clear();
                return;
            }
        }
    }
    static int pos(ArrayList p, int n) {
        return ((Integer) p.get(n)).intValue();
    }
    static boolean opcodes(Method m, ArrayList p, int[] expected) throws IOException {
        if (p.size() != expected.length)
            return false;
        for (int i = 0; i < expected.length; i++)
            if (u1(m.code, pos(p, i)) != expected[i])
                return false;
        return true;
    }
    static boolean suffixGate(ClassFile c, Method m, ArrayList p) throws IOException {
        if (!"(Ljava/lang/String;)Z".equals(m.desc) || (m.access & 8) != 0)
            return false;
        // Exact null-safe OR of two String.endsWith predicates; ldc widths vary.
        if (p.size() != 14)
            return false;
        int[] shape = {43, 198, 43, -1, 182, 154, 43, -1, 182, 153, 4, 172, 3, 172};
        for (int i = 0; i < shape.length; i++)
            if (shape[i] >= 0 && u1(m.code, pos(p, i)) != shape[i])
                return false;
        HashSet suffixes = new HashSet();
        for (int i = 3; i <= 7; i += 4) {
            int at = pos(p, i), op = u1(m.code, at);
            if (op != 18 && op != 19)
                return false;
            Object value = c.constant(op == 18 ? u1(m.code, at + 1) : u2(m.code, at + 1));
            if (!(value instanceof String))
                return false;
            suffixes.add(value);
            Ref call = c.ref(u2(m.code, pos(p, i + 1) + 1));
            if (!"java/lang/String".equals(call.owner) || !"endsWith".equals(call.name)
                || !"(Ljava/lang/String;)Z".equals(call.desc))
                return false;
        }
        if (suffixes.size() != 2 || !suffixes.contains(".azw3") || !suffixes.contains(".kdl"))
            return false;
        int a = pos(p, 1), b = pos(p, 5), d = pos(p, 9);
        return a + (short) u2(m.code, a + 1) == pos(p, 12)
            && b + (short) u2(m.code, b + 1) == pos(p, 10)
            && d + (short) u2(m.code, d + 1) == pos(p, 12);
    }
    void structuralMethods(ClassFile c, Method m, ArrayList p) throws IOException {
        byte[] b = m.code;
        if ("java/util/AbstractList".equals(c.superName)) {
            if ("get".equals(m.name) && "(I)Ljava/lang/Object;".equals(m.desc)
                && opcodes(m, p, new int[] {42, 180, 27, 50, 176})) {
                Ref r = c.ref(u2(b, pos(p, 1) + 1));
                if (c.name.equals(r.owner) && STRINGS.equals(r.desc))
                    c.listGetField = r.toString();
            }
            if ("size".equals(m.name) && "()I".equals(m.desc)
                && opcodes(m, p, new int[] {42, 180, 190, 172})) {
                Ref r = c.ref(u2(b, pos(p, 1) + 1));
                if (c.name.equals(r.owner) && STRINGS.equals(r.desc))
                    c.listSizeField = r.toString();
            }
        }
        if ("()Ljava/util/List;".equals(m.desc) && (m.access & 8) == 0
            && opcodes(m, p, new int[] {187, 89, 42, 183, 176})) {
            String helper = c.className(u2(b, pos(p, 0) + 1));
            Ref ctor = c.ref(u2(b, pos(p, 3) + 1));
            if (helper.equals(ctor.owner) && "<init>".equals(ctor.name)
                && ("(L" + c.name + ";)V").equals(ctor.desc)) {
                c.listGetters.add(new String[] {m.toString(), helper, ctor.desc});
                out.println(" LIST_CONSTRUCTION getter=" + c.name + "." + m + " helper=" + helper
                    + " constructor=" + ctor.desc);
            }
        }
        if (suffixGate(c, m, p)) {
            c.suffixGates.add(m.toString());
            out.println(
                " SUFFIX_OR_GATE method=" + c.name + "." + m + " null=false suffixes=[.azw3,.kdl]");
        }
    }

    void inspect(String jar, String entry, ClassFile c) throws IOException {
        if (++candidates > MAX_CANDIDATES)
            throw new IOException("candidate limit exceeded");
        out.println("CANDIDATE jar=" + jar + " entry=" + entry + " class=" + c.name
            + " major=" + c.major + " super=" + c.superName);
        HashMap initialized =
            new HashMap(); // exact straight-line literal-array initialization only
        HashMap readGetters = new HashMap();
        HashMap writes = new HashMap();
        for (int i = 0; i < c.fields.size(); i++) {
            Object[] f = (Object[]) c.fields.get(i);
            Ref r = (Ref) f[1];
            if (arrayOrList(r.desc))
                out.println(" FIELD access=0x" + Integer.toHexString(((Integer) f[0]).intValue())
                    + " " + r);
        }
        for (int i = 0; i < c.methods.size(); i++) {
            Method m = (Method) c.methods.get(i);
            if (m.code == null)
                continue;
            byte[] b = m.code;
            ArrayList stack = new ArrayList();
            ArrayList instructions = new ArrayList();
            ArrayList details = new ArrayList();
            HashSet literals = new HashSet();
            boolean gate = "(Ljava/lang/String;)Z".equals(m.desc);
            for (int p = 0; p < b.length;) {
                int op = u1(b, p), end = next(b, p);
                instructions.add(new Integer(p));
                Object value = null;
                if (op == 18 || op == 19 || op == 20) {
                    value = c.constant(op == 18 ? u1(b, p + 1) : u2(b, p + 1));
                    if (value instanceof String) {
                        String s = (String) value;
                        if (extension(s, "azw3") || extension(s, "kdl") || extension(s, "mobi")
                            || extension(s, "epub")) {
                            literals.add(s);
                            details.add("  @" + p + " LDC cp="
                                + (op == 18 ? u1(b, p + 1) : u2(b, p + 1)) + " string=" + s);
                        }
                    }
                    stack.add(value);
                } else if (op >= 2 && op <= 8)
                    stack.add(new Integer(op - 3));
                else if (op == 42)
                    stack.add(THIS);
                else if (op >= 43 && op <= 45)
                    stack.add(OTHER_LOCAL);
                else if (op == 16)
                    stack.add(new Integer((byte) u1(b, p + 1)));
                else if (op == 17)
                    stack.add(new Integer((short) u2(b, p + 1)));
                else if (op == 87 || op == 88)
                    discard(stack, op == 87 ? 1 : 2);
                else if (op == 89) {
                    if (stack.isEmpty() || stack.get(stack.size() - 1) == CATEGORY2)
                        stack.clear();
                    else
                        stack.add(stack.get(stack.size() - 1));
                } else if (op == 189) {
                    Object count = pop(stack);
                    String type = c.className(u2(b, p + 1));
                    if (count instanceof Integer && "java/lang/String".equals(type)
                        && ((Integer) count).intValue() >= 0 && ((Integer) count).intValue() <= 128)
                        stack.add(new ArrayValue(((Integer) count).intValue()));
                    else {
                        stack.clear();
                        stack.add(null);
                    }
                } else if (op == 83) {
                    Object item = pop(stack), index = pop(stack), array = pop(stack);
                    if (array instanceof ArrayValue && index instanceof Integer
                        && item instanceof String) {
                        int ix = ((Integer) index).intValue();
                        ArrayValue a = (ArrayValue) array;
                        if (ix >= 0 && ix < a.values.length)
                            a.values[ix] = (String) item;
                        else
                            stack.clear();
                    } else
                        stack.clear();
                } else if (op == 178 || op == 179 || op == 180 || op == 181) {
                    Ref r = c.ref(u2(b, p + 1));
                    if (op == 179 || op == 181) {
                        Integer count = (Integer) writes.get(r.toString());
                        writes.put(
                            r.toString(), new Integer(count == null ? 1 : count.intValue() + 1));
                    }
                    if (op == 181 && "<init>".equals(m.name) && c.name.equals(r.owner)) {
                        String key = m.desc + " " + r.toString();
                        Integer count = (Integer) c.instanceWrites.get(key);
                        c.instanceWrites.put(key, new Integer(count == null ? 1 : count.intValue() + 1));
                    }
                    if (arrayOrList(r.desc))
                        details.add("  @" + p + " "
                            + (op == 178        ? "GETSTATIC "
                                    : op == 179 ? "PUTSTATIC "
                                    : op == 180 ? "GETFIELD "
                                                : "PUTFIELD ")
                            + r);
                    if (op == 178)
                        stack.add(r);
                    else if (op == 180) {
                        pop(stack);
                        stack.add(r);
                    } else {
                        Object assigned = pop(stack);
                        Object receiver = op == 181 ? pop(stack) : null;
                        if (assigned instanceof ArrayValue && STRINGS.equals(r.desc)) {
                            ArrayValue a = (ArrayValue) assigned;
                            details.add("  @" + p + " LITERAL_ARRAY " + r + " values=" + a);
                            if (op == 179 && a.target() && "<clinit>".equals(m.name)
                                && c.name.equals(r.owner)) {
                                if (initialized.containsKey(r.toString()))
                                    initialized.put(r.toString(), Boolean.FALSE);
                                else
                                    initialized.put(r.toString(), a);
                            }
                            if (op == 181 && receiver == THIS && a.target()
                                && "<init>".equals(m.name) && c.name.equals(r.owner)) {
                                String key = m.desc + " " + r.toString();
                                if (c.instanceArrays.containsKey(key))
                                    c.instanceArrays.put(key, Boolean.FALSE);
                                else
                                    c.instanceArrays.put(key, a);
                            }
                        }
                    }
                } else if (op == 176) {
                    pop(stack);
                    stack.clear();
                } else if (op >= 182 && op <= 185) {
                    Ref r = c.ref(u2(b, p + 1));
                    if (gate || getter(m) || "<clinit>".equals(m.name))
                        details.add("  @" + p + " INVOKE " + r);
                    stack.clear();
                } else if (op != 0)
                    stack.clear();
                p = end;
            }
            structuralMethods(c, m, instructions);
            if (getter(m) && instructions.size() == 2) {
                int p = ((Integer) instructions.get(0)).intValue(),
                    q = ((Integer) instructions.get(1)).intValue();
                if (u1(b, p) == 178 && u1(b, q) == 176) {
                    Ref r = c.ref(u2(b, p + 1));
                    if (c.name.equals(r.owner) && ("()" + r.desc).equals(m.desc)) {
                        details.add("  SIMPLE_STATIC_GETTER field=" + r);
                        ArrayList names = (ArrayList) readGetters.get(r.toString());
                        if (names == null) {
                            names = new ArrayList();
                            readGetters.put(r.toString(), names);
                        }
                        names.add(m.toString());
                    }
                }
            }
            if (!literals.isEmpty() || getter(m) || gate || !details.isEmpty()) {
                out.println(" METHOD " + m + " access=0x" + Integer.toHexString(m.access)
                    + " code=" + b.length + " instructions=" + instructions.size() + " kind="
                    + (getter(m)   ? "zero-arg-extensions"
                            : gate ? "string-boolean-gate"
                                   : "other"));
                for (int j = 0; j < details.size(); j++)
                    out.println(details.get(j));
            }
        }
        for (Iterator it = initialized.keySet().iterator(); it.hasNext();) {
            String key = (String) it.next();
            Object array = initialized.get(key);
            ArrayList getters = (ArrayList) readGetters.get(key);
            Integer count = (Integer) writes.get(key);
            boolean finalStatic = false;
            for (int f = 0; f < c.fields.size(); f++) {
                Object[] field = (Object[]) c.fields.get(f);
                if (((Ref) field[1]).toString().equals(key)
                    && (((Integer) field[0]).intValue() & 24) == 24)
                    finalStatic = true;
            }
            if (array instanceof ArrayValue && finalStatic && count != null && count.intValue() == 1
                && getters != null) {
                // Every complete getter participates in global uniqueness.
                for (int g = 0; g < getters.size(); g++) {
                    resolved.add(new Match(jar, c, (String) getters.get(g), null));
                    out.println("STATIC_ARRAY class=" + c.name + " field=" + key
                        + " getter=" + getters.get(g) + " values=" + array);
                }
            }
        }
    }
    void resolveLists(String jar, HashMap within) throws IOException {
        ArrayList matches = new ArrayList();
        for (Iterator it = within.values().iterator(); it.hasNext();) {
            ClassFile owner = (ClassFile) it.next();
            if (owner.suffixGates.size() != 1)
                continue;
            for (int i = 0; i < owner.listGetters.size(); i++) {
                String[] getter = (String[]) owner.listGetters.get(i);
                ClassFile helper = (ClassFile) within.get(getter[1]);
                if (helper == null || !"java/util/AbstractList".equals(helper.superName)
                    || helper.listGetField == null
                    || !helper.listGetField.equals(helper.listSizeField))
                    continue;
                String fieldKey = getter[2] + " " + helper.listGetField;
                Object values = helper.instanceArrays.get(fieldKey);
                Integer writes = (Integer) helper.instanceWrites.get(fieldKey);
                if (!(values instanceof ArrayValue) || writes == null || writes.intValue() != 1)
                    continue;
                boolean finalField = false;
                for (int f = 0; f < helper.fields.size(); f++) {
                    Object[] field = (Object[]) helper.fields.get(f);
                    int access = ((Integer) field[0]).intValue();
                    if (((Ref) field[1]).toString().equals(helper.listGetField)
                        && (access & 16) != 0 && (access & 8) == 0)
                        finalField = true;
                }
                if (!finalField)
                    continue;
                matches.add(new Match(jar, owner, getter[0], (String) owner.suffixGates.get(0)));
                out.println("LIST_ARRAY owner=" + owner.name + " getter=" + getter[0]
                    + " helper=" + helper.name + " field=" + helper.listGetField
                    + " gate=" + owner.suffixGates.get(0));
            }
        }
        // All complete semantic matches participate in final scope uniqueness.
        resolved.addAll(matches);
    }
    static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int n;
        while ((n = in.read(buffer)) != -1) {
            if (out.size() + n > MAX_CLASS_BYTES)
                throw new IOException("class too large");
            out.write(buffer, 0, n);
        }
        return out.toByteArray();
    }
    void scanJar(File file) throws IOException {
        if (++jars > MAX_JARS)
            throw new IOException("jar limit exceeded");
        int before = classes;
        long sourceSize = file.length(), sourceModified = file.lastModified();
        HashMap within = new HashMap();
        ZipFile zip = new ZipFile(file);
        try {
            for (Enumeration en = zip.entries(); en.hasMoreElements();) {
                ZipEntry entry = (ZipEntry) en.nextElement();
                if (!entry.getName().endsWith(".class"))
                    continue;
                if (++classes > MAX_CLASSES)
                    throw new IOException("class limit exceeded");
                InputStream input = zip.getInputStream(entry);
                byte[] bytes;
                try {
                    bytes = read(input);
                } finally {
                    input.close();
                }
                bytesRead += bytes.length;
                if (bytesRead > 1024L * 1024 * 1024)
                    throw new IOException("scan size limit exceeded");
                ClassFile c = new ClassFile(bytes);
                if (c.shortlist()) {
                    if (!entry.getName().equals(c.name + ".class"))
                        throw new IOException("class entry/name mismatch");
                    if (within.containsKey(c.name))
                        throw new IOException("duplicate candidate class " + c.name);
                    inspect(file.getCanonicalPath(), entry.getName(), c);
                    within.put(c.name, c);
                }
            }
            if (file.length() != sourceSize || file.lastModified() != sourceModified)
                throw new IOException("source JAR changed during discovery: " + file);
            resolveLists(file.getCanonicalPath(), within);
        } finally {
            zip.close();
        }
        out.println("JAR file=" + file + " classes=" + (classes - before));
        out.flush();
    }
    void walk(File file, int depth) throws IOException {
        if (depth > 16)
            throw new IOException("directory depth exceeded");
        String name = file.getCanonicalPath();
        if (!visited.add(name))
            return;
        if (visited.size() > 100000)
            throw new IOException("file limit exceeded");
        if (file.isDirectory()) {
            File[] files = file.listFiles();
            if (files == null)
                throw new IOException("cannot list " + file);
            for (int i = 0; i < files.length; i++)
                walk(files[i], depth + 1);
        } else if (file.isFile() && file.getName().toLowerCase(Locale.ENGLISH).endsWith(".jar"))
            scanJar(file);
        else if (!file.exists())
            throw new IOException("missing input " + file);
    }
    Match discover(String[] paths) throws IOException {
        long started = System.currentTimeMillis();
        for (int i = 0; i < paths.length; i++)
            walk(new File(paths[i]), 0);
        out.println("TOTAL jars=" + jars + " classes=" + classes + " candidates=" + candidates
            + " matches=" + resolved.size()
            + " elapsed_ms=" + (System.currentTimeMillis() - started));
        if (resolved.size() != 1)
            throw new IOException("expected one complete semantic match; found " + resolved.size());
        return (Match) resolved.get(0);
    }
}
