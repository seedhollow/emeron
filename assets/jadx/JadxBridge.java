// emeron's bridge to jadx (https://github.com/skylot/jadx, Apache-2.0).
//
// One long-lived JVM per opened APK, so the APK is loaded once and every class
// after that decompiles in milliseconds -- the same model as jadx-gui. emeron
// runs it with the jadx jar on the class path, in source-file mode:
//
//   java -cp jadx-<ver>-all.jar JadxBridge.java [--deobf] app.apk [split.apk...]
//
// Protocol, UTF-8 over stdin/stdout. A request is one line:
//
//   <id> TAB <command> [TAB <arg>]...
//
// A response is a header line and a payload of exactly <length> bytes:
//
//   @<id> <status> <length> LF <payload>
//
// status is "ok" or "err" (final), or "part" for a streamed chunk of a long
// request (search, usages) that more will follow. Request 0 is the load
// itself. Positions in code are UTF-8 byte offsets, not Java char indices.
// Nothing else may write to stdout: System.out is pointed at stderr before
// jadx (and its logger) start.

import jadx.api.ICodeCache;
import jadx.api.ICodeInfo;
import jadx.api.JadxArgs;
import jadx.api.JadxDecompiler;
import jadx.api.JavaClass;
import jadx.api.JavaNode;
import jadx.api.ResourceFile;
import jadx.api.metadata.ICodeAnnotation;
import jadx.api.metadata.annotations.NodeDeclareRef;
import jadx.core.dex.info.AccessInfo;
import jadx.core.xmlgen.ResContainer;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public final class JadxBridge {
    private static final int MAX_SEARCH_HITS = 5000;
    private static final int MAX_LINE = 240;

    // Packages that are almost always a library, not the app: what "skip
    // libraries" leaves out of a search.
    private static final String[] LIBRARIES = {
        "android.", "androidx.", "kotlin.", "kotlinx.", "java.", "javax.", "dalvik.", "j$.",
        "org.jetbrains.", "org.intellij.", "org.json.", "org.apache.", "org.chromium.", "org.slf4j.",
        "org.greenrobot.", "com.google.", "com.android.billingclient.", "com.android.installreferrer.",
        "com.squareup.", "okhttp3.", "okio.", "retrofit2.", "dagger.", "hilt_aggregated_deps.",
        "io.reactivex.", "rx.", "io.grpc.", "io.ktor.", "io.sentry.", "io.flutter.", "coil.",
        "com.bumptech.glide.", "com.airbnb.lottie.", "com.facebook.", "com.fasterxml.", "timber.",
        "com.applovin.", "com.unity3d.", "com.ironsource.", "com.appsflyer.", "com.adjust.",
        "com.huawei.hms.", "com.yalantis.", "_COROUTINE.",
    };

    private static boolean isLibrary(String name) {
        for (String prefix : LIBRARIES) {
            if (name.startsWith(prefix)) return true;
        }
        return false;
    }

    private static OutputStream out;
    private static JadxDecompiler jadx;
    private static final Map<String, JavaClass> classes = new ConcurrentHashMap<>();
    private static final Map<String, ResourceFile> resources = new ConcurrentHashMap<>();
    private static final Map<String, ResContainer> resCache = new ConcurrentHashMap<>();
    private static final Map<Integer, AtomicBoolean> cancels = new ConcurrentHashMap<>();
    private static int threads;

    // Decompiled code, bounded by total length so a whole-app search on a huge
    // APK cannot run the JVM out of heap. Least recently used goes first.
    private static final class LruCodeCache implements ICodeCache {
        private final long budget;
        private long used;
        private final LinkedHashMap<String, ICodeInfo> map = new LinkedHashMap<>(256, 0.75f, true);

        LruCodeCache(long budgetChars) { this.budget = budgetChars; }

        @Override public synchronized void add(String id, ICodeInfo info) {
            ICodeInfo old = map.put(id, info);
            if (old != null) used -= old.getCodeStr().length();
            used += info.getCodeStr().length();
            var it = map.entrySet().iterator();
            while (used > budget && it.hasNext()) {
                var e = it.next();
                if (e.getKey().equals(id)) continue;
                used -= e.getValue().getCodeStr().length();
                it.remove();
            }
        }
        @Override public synchronized void remove(String id) {
            ICodeInfo old = map.remove(id);
            if (old != null) used -= old.getCodeStr().length();
        }
        @Override public synchronized ICodeInfo get(String id) {
            ICodeInfo info = map.get(id);
            return info != null ? info : ICodeInfo.EMPTY;
        }
        @Override public synchronized String getCode(String id) {
            ICodeInfo info = map.get(id);
            return info != null ? info.getCodeStr() : null;
        }
        @Override public synchronized boolean contains(String id) { return map.containsKey(id); }
        @Override public synchronized void close() { map.clear(); used = 0; }
    }

    public static void main(String[] args) throws Exception {
        out = new FileOutputStream(FileDescriptor.out);
        System.setOut(System.err);

        boolean deobf = false;
        List<File> inputs = new ArrayList<>();
        for (String a : args) {
            if (a.equals("--deobf")) deobf = true;
            else inputs.add(new File(a));
        }
        threads = Math.max(2, Runtime.getRuntime().availableProcessors() - 1);

        JadxArgs jargs = new JadxArgs();
        jargs.setInputFiles(inputs);
        jargs.setDeobfuscationOn(deobf);
        jargs.setShowInconsistentCode(true);
        jargs.setThreadsCount(threads);
        long heap = Runtime.getRuntime().maxMemory();
        jargs.setCodeCache(new LruCodeCache(Math.max(8L << 20, heap / 8)));
        long start = System.nanoTime();
        try {
            jadx = new JadxDecompiler(jargs);
            jadx.load();
        } catch (Throwable t) {
            send(0, "err", describe(t));
            System.exit(1);
        }
        for (JavaClass c : jadx.getClasses()) classes.put(c.getRawName(), c);
        for (ResourceFile r : jadx.getResources()) resources.put(r.getDeobfName(), r);
        send(0, "ok", "jadx\t" + JadxDecompiler.getVersion() + "\nclasses\t" + classes.size()
                + "\nresources\t" + resources.size() + "\nms\t"
                + (System.nanoTime() - start) / 1_000_000 + "\n");

        // Opening code must not wait behind a whole-app search.
        ExecutorService fast = Executors.newFixedThreadPool(2);
        ExecutorService slow = Executors.newSingleThreadExecutor();
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        String line;
        while ((line = in.readLine()) != null) {
            String[] p = line.split("\t", -1);
            if (p.length < 2) continue;
            final int id;
            try {
                id = Integer.parseInt(p[0]);
            } catch (NumberFormatException e) {
                continue;
            }
            String cmd = p[1];
            if (cmd.equals("quit")) break;
            if (cmd.equals("cancel")) {
                AtomicBoolean flag = p.length > 2 ? cancels.get(Integer.parseInt(p[2])) : null;
                if (flag != null) flag.set(true);
                continue;
            }
            AtomicBoolean cancelled = new AtomicBoolean();
            cancels.put(id, cancelled);
            boolean long_ = cmd.equals("search") || cmd.equals("usages");
            (long_ ? slow : fast).execute(() -> {
                try {
                    handle(id, cmd, p, cancelled);
                } catch (Throwable t) {
                    send(id, "err", describe(t));
                } finally {
                    cancels.remove(id);
                }
            });
        }
        System.exit(0);
    }

    private static void handle(int id, String cmd, String[] p, AtomicBoolean cancelled) throws Exception {
        switch (cmd) {
            case "classes" -> send(id, "ok", classList());
            case "code" -> send(id, "ok", code(cls(p, 2)));
            case "smali" -> send(id, "ok", cls(p, 2).getSmali());
            case "resolve" -> send(id, "ok", resolve(cls(p, 2), Integer.parseInt(arg(p, 3))));
            case "usages" -> usages(id, cls(p, 2), Integer.parseInt(arg(p, 3)), cancelled);
            case "search" -> search(id, arg(p, 2), arg(p, 3), arg(p, 4), p.length > 5 ? p[5] : "", cancelled);
            case "resources" -> send(id, "ok", resourceList());
            case "res" -> send(id, "ok", resource(arg(p, 2)));
            default -> send(id, "err", "unknown command: " + cmd);
        }
    }

    // --- commands -------------------------------------------------------------------------

    // raw name TAB display name TAB flags, one top-level class per line.
    private static String classList() {
        StringBuilder sb = new StringBuilder(classes.size() * 64);
        for (JavaClass c : jadx.getClasses()) {
            sb.append(c.getRawName()).append('\t').append(c.getFullName()).append('\t');
            AccessInfo a = c.getAccessInfo();
            if (a.isAnnotation()) sb.append('a');
            else if (a.isInterface()) sb.append('i');
            else if (a.isEnum()) sb.append('e');
            else if (a.isAbstract()) sb.append('b');
            if (c.isNoCode()) sb.append('n');
            sb.append('\n');
        }
        return sb.toString();
    }

    // <code length> LF <code> then "pos TAB kind" lines: c/m/f/v a reference to
    // a class, method, field or variable; C/M/F/V where one is declared.
    private static byte[] code(JavaClass cls) {
        ICodeInfo info = cls.getCodeInfo();
        String code = info.getCodeStr();
        int[] off = utf8Offsets(code);
        StringBuilder meta = new StringBuilder();
        for (Map.Entry<Integer, ICodeAnnotation> e : info.getCodeMetadata().getAsMap().entrySet()) {
            char kind = kindOf(e.getValue());
            int pos = e.getKey();
            if (kind == 0 || pos < 0 || pos >= code.length()) continue;
            meta.append(off[pos]).append('\t').append(kind).append('\n');
        }
        byte[] body = code.getBytes(StandardCharsets.UTF_8);
        byte[] head = (body.length + "\n").getBytes(StandardCharsets.UTF_8);
        byte[] tail = meta.toString().getBytes(StandardCharsets.UTF_8);
        byte[] all = new byte[head.length + body.length + tail.length];
        System.arraycopy(head, 0, all, 0, head.length);
        System.arraycopy(body, 0, all, head.length, body.length);
        System.arraycopy(tail, 0, all, head.length + body.length, tail.length);
        return all;
    }

    // Where the thing at `bytePos` is declared: raw top class TAB byte position
    // in that class's code TAB full name.
    private static String resolve(JavaClass cls, int bytePos) {
        ICodeInfo info = cls.getCodeInfo();
        JavaNode node = nodeAt(info, bytePos);
        if (node == null) throw new IllegalArgumentException("not a reference");
        JavaClass top = node.getTopParentClass();
        if (top == null) throw new IllegalArgumentException("declared outside this APK");
        classes.putIfAbsent(top.getRawName(), top);
        ICodeInfo target = top.getCodeInfo();
        int pos = Math.max(0, node.getDefPos());
        return top.getRawName() + "\t" + utf8Offset(target.getCodeStr(), pos) + "\t" + node.getFullName();
    }

    // Streams "raw TAB display TAB bytePos TAB line" for every use of the node at
    // `bytePos`, one class at a time.
    private static void usages(int id, JavaClass cls, int bytePos, AtomicBoolean cancelled) {
        ICodeInfo info = cls.getCodeInfo();
        JavaNode node = nodeAt(info, bytePos);
        if (node == null) throw new IllegalArgumentException("not a class, method, field or variable");
        Set<JavaClass> seen = new HashSet<>();
        int count = 0;
        StringBuilder sb = new StringBuilder();
        sb.append("N\t").append(node.getFullName()).append('\n');
        for (JavaNode use : node.getUseIn()) {
            if (cancelled.get()) break;
            JavaClass top = use.getTopParentClass();
            if (top == null || !seen.add(top)) continue;
            classes.putIfAbsent(top.getRawName(), top);
            ICodeInfo ci = top.getCodeInfo();
            String code = ci.getCodeStr();
            for (int pos : top.getUsePlacesFor(ci, node)) {
                appendHit(sb, top, code, pos);
                ++count;
            }
            if (sb.length() > 32 * 1024) {
                send(id, "part", sb.toString());
                sb.setLength(0);
            }
        }
        sb.append("T\t").append(count).append('\n');
        send(id, "ok", sb.toString());
    }

    // Full-text search over decompiled code. flags: 'i' ignore case, 'l' skip
    // libraries -- except under `keep`, the app's own package (a Google app is
    // not a library). `scope` is a package prefix, empty for everything.
    // Streams hits and "P TAB done TAB total" progress.
    private static void search(int id, String flags, String scope, String query, String keep,
            AtomicBoolean cancelled)
            throws InterruptedException {
        boolean ignoreCase = flags.indexOf('i') >= 0;
        boolean skipLibraries = flags.indexOf('l') >= 0;
        String needle = ignoreCase ? query.toLowerCase(Locale.ROOT) : query;
        if (needle.isEmpty()) throw new IllegalArgumentException("empty query");
        List<JavaClass> todo = new ArrayList<>();
        for (JavaClass c : jadx.getClasses()) {
            String name = c.getFullName();
            if (!scope.isEmpty() && !name.startsWith(scope)) continue;
            if (skipLibraries && isLibrary(name) && (keep.isEmpty() || !name.startsWith(keep))) continue;
            todo.add(c);
        }
        int total = todo.size();
        AtomicInteger next = new AtomicInteger();
        AtomicInteger done = new AtomicInteger();
        AtomicInteger hits = new AtomicInteger();
        StringBuilder pending = new StringBuilder();
        Thread[] workers = new Thread[threads];
        for (int w = 0; w < workers.length; ++w) {
            workers[w] = new Thread(() -> {
                int i;
                while (!cancelled.get() && hits.get() < MAX_SEARCH_HITS && (i = next.getAndIncrement()) < total) {
                    JavaClass c = todo.get(i);
                    StringBuilder local = new StringBuilder();
                    try {
                        String code = c.getCode();
                        String hay = ignoreCase ? code.toLowerCase(Locale.ROOT) : code;
                        int from = 0;
                        int lastLine = -1;
                        int at;
                        while ((at = hay.indexOf(needle, from)) >= 0) {
                            int lineStart = code.lastIndexOf('\n', at) + 1;
                            if (lineStart != lastLine) {
                                appendHit(local, c, code, at);
                                lastLine = lineStart;
                                if (hits.incrementAndGet() >= MAX_SEARCH_HITS) break;
                            }
                            from = at + needle.length();
                        }
                    } catch (Throwable t) {
                        // A class jadx cannot decompile is just not a hit.
                    }
                    done.incrementAndGet();
                    if (local.length() > 0) {
                        synchronized (pending) { pending.append(local); }
                    }
                }
            });
            workers[w].setDaemon(true);
            workers[w].start();
        }
        boolean running = true;
        while (running) {
            running = false;
            for (Thread t : workers) {
                t.join(200);
                if (t.isAlive()) {
                    running = true;
                    break;
                }
            }
            String chunk;
            synchronized (pending) {
                chunk = pending.toString();
                pending.setLength(0);
            }
            String progress = "P\t" + done.get() + "\t" + total + "\n";
            if (running) send(id, "part", chunk + progress);
            else send(id, "ok", chunk + progress + (hits.get() >= MAX_SEARCH_HITS ? "L\n" : ""));
        }
    }

    private static String resourceList() {
        StringBuilder sb = new StringBuilder();
        for (ResourceFile r : jadx.getResources()) {
            sb.append(r.getDeobfName()).append('\t').append(r.getType().name()).append('\n');
        }
        return sb.toString();
    }

    // "t" LF text, "l" LF child names (resources.arsc), or "b TAB size" LF for
    // binary content shown only by size.
    private static String resource(String name) {
        ResContainer c = resCache.get(name);
        if (c == null) {
            ResourceFile r = resources.get(name);
            if (r == null) throw new IllegalArgumentException("no resource " + name);
            c = r.loadContent();
            resCache.put(name, c);
        }
        for (int depth = 0; c.getDataType() == ResContainer.DataType.RES_LINK && depth < 4; ++depth) {
            c = c.getResLink().loadContent();
        }
        switch (c.getDataType()) {
            case TEXT:
                return "t\n" + c.getText().getCodeStr();
            case RES_TABLE: {
                StringBuilder sb = new StringBuilder("l\n");
                for (ResContainer sub : c.getSubFiles()) {
                    resCache.putIfAbsent(sub.getName(), sub);
                    sb.append(sub.getName()).append('\n');
                }
                return sb.toString();
            }
            case DECODED_DATA:
                return "b\t" + c.getDecodedData().length + "\n";
            default:
                return "b\t0\n";
        }
    }

    // --- helpers ----------------------------------------------------------------------------

    private static JavaClass cls(String[] p, int i) {
        String raw = arg(p, i);
        JavaClass c = classes.get(raw);
        if (c == null) {
            c = jadx.searchJavaClassOrItsParentByOrigFullName(raw);
            if (c == null) throw new IllegalArgumentException("no class " + raw);
            c = c.getTopParentClass();
            classes.putIfAbsent(c.getRawName(), c);
        }
        return c;
    }

    private static String arg(String[] p, int i) {
        if (i >= p.length) throw new IllegalArgumentException("missing argument");
        return p[i];
    }

    private static JavaNode nodeAt(ICodeInfo info, int bytePos) {
        int pos = charIndex(info.getCodeStr(), bytePos);
        ICodeAnnotation ann = info.getCodeMetadata().getAt(pos);
        if (ann == null) return null;
        return jadx.getJavaNodeByCodeAnnotation(info, ann);
    }

    private static char kindOf(ICodeAnnotation ann) {
        if (ann instanceof NodeDeclareRef decl) {
            return switch (decl.getNode().getAnnType()) {
                case CLASS -> 'C';
                case METHOD -> 'M';
                case FIELD -> 'F';
                case VAR -> 'V';
                default -> 0;
            };
        }
        return switch (ann.getAnnType()) {
            case CLASS -> 'c';
            case METHOD -> 'm';
            case FIELD -> 'f';
            case VAR, VAR_REF -> 'v';
            default -> 0;
        };
    }

    private static void appendHit(StringBuilder sb, JavaClass cls, String code, int pos) {
        int start = code.lastIndexOf('\n', Math.max(0, pos - 1)) + 1;
        if (pos == 0) start = 0;
        int end = code.indexOf('\n', pos);
        if (end < 0) end = code.length();
        String line = code.substring(start, end).strip();
        if (line.length() > MAX_LINE) line = line.substring(0, MAX_LINE);
        sb.append(cls.getRawName()).append('\t').append(cls.getFullName()).append('\t')
                .append(utf8Offset(code, pos)).append('\t').append(line.replace('\t', ' ')).append('\n');
    }

    private static int utf8Len(char ch) {
        if (ch < 0x80) return 1;
        if (ch < 0x800) return 2;
        if (Character.isSurrogate(ch)) return 2;  // a pair is 4 bytes
        return 3;
    }

    private static int[] utf8Offsets(String s) {
        int[] off = new int[s.length() + 1];
        int o = 0;
        for (int i = 0; i < s.length(); ++i) {
            off[i] = o;
            o += utf8Len(s.charAt(i));
        }
        off[s.length()] = o;
        return off;
    }

    private static int utf8Offset(String s, int charIndex) {
        int o = 0;
        int n = Math.min(charIndex, s.length());
        for (int i = 0; i < n; ++i) o += utf8Len(s.charAt(i));
        return o;
    }

    private static int charIndex(String s, int bytePos) {
        int o = 0;
        for (int i = 0; i < s.length(); ++i) {
            if (o >= bytePos) return i;
            o += utf8Len(s.charAt(i));
        }
        return s.length();
    }

    private static String describe(Throwable t) {
        String m = t.getMessage();
        return t.getClass().getSimpleName() + (m != null ? ": " + m : "");
    }

    private static void send(int id, String status, String payload) {
        send(id, status, payload.getBytes(StandardCharsets.UTF_8));
    }

    private static synchronized void send(int id, String status, byte[] payload) {
        try {
            out.write(("@" + id + " " + status + " " + payload.length + "\n").getBytes(StandardCharsets.UTF_8));
            out.write(payload);
            out.flush();
        } catch (IOException e) {
            System.exit(2);  // emeron went away
        }
    }
}
