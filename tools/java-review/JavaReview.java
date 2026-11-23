// JavaReview.java — Java の「どこを・何を・何を使って書くか」を出す reviewer
//
// Java パーサは書かない。JDK の javac (Compiler Tree API) に
// parse / analyze させ、解決済みの Element / TypeMirror / 位置だけを使う。
//
//   java JavaReview.java <dir>
//   java JavaReview.java <dir> --next
//
// 出すもの:
//   - 未実装・スタブのメソッド (file:line:column, シグネチャ, マーカー)
//   - 契約 (override 元 / interface 宣言) の場所と説明
//   - 呼び出し元 (誰がそのメソッドを必要としているか)
//   - テストの期待値 (3 引数の expect(label, expected, actual) 形状)
//   - 使える材料 (引数・戻り値の型のメソッド/フィールドを実ファイル位置つきで)
//   - そのまま書ける骨組み

import com.sun.source.tree.*;
import com.sun.source.util.*;

import javax.lang.model.element.*;
import javax.lang.model.type.*;
import javax.lang.model.util.*;
import javax.tools.*;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.Pattern;
import java.util.stream.Collectors;

public final class JavaReview {

    // ------------------------------------------------------------------
    // データ
    // ------------------------------------------------------------------

    static final class Call {
        ExecutableElement target;
        String caller;
        String where;
        String text;
        String callerSource;
        boolean contract;
    }

    static final class Expect {
        String label;
        String expected;
        String actual;
        String where;
        String caller;
        String callerSource;
    }

    static final class MethodInfo {
        ExecutableElement element;
        MethodTree tree;
        CompilationUnitTree cu;
        TypeElement owner;
        String file;
        int line;
        int column;
        int startLine;
        int bodyLine;
        int bodyEndLine;
        long start;
        long end;
        String source;
        String bodySource;
        double stub;
        String marker;
        final List<ExecutableElement> contracts = new ArrayList<>();
        final List<Call> calls = new ArrayList<>();
        final List<Expect> expects = new ArrayList<>();

        String qualifiedName() {
            return owner.getQualifiedName() + "." + element.getSimpleName();
        }

        String signature() {
            String ret = tree.getReturnType() == null ? "" : tree.getReturnType().toString() + " ";
            String params = tree.getParameters().stream()
                .map(p -> p.getType() + " " + p.getName())
                .collect(Collectors.joining(", "));
            return ret + element.getSimpleName() + "(" + params + ")";
        }
    }

    // ------------------------------------------------------------------
    // 設定
    // ------------------------------------------------------------------

    static final Pattern MARKER = Pattern.compile(
        "(?i)\\b(todo|fixme|task\\s*\\d+|implement|not implemented|unimplemented)\\b"
        + "|未実装|ここを実装|実装する");

    static final Pattern NEUTRAL = Pattern.compile(
        "^\\s*(0|0\\.0|0L|0\\.0f|false|null|\"\"|\\(\\)|\\{\\}|\\[\\])\\s*$");


    static Path ROOT;

    // ------------------------------------------------------------------
    // main
    // ------------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        if (args.length == 0 || args[0].equals("-h") || args[0].equals("--help")) {
            System.err.println("usage: java JavaReview.java <dir> [--next|--map]");
            System.exit(2);
        }
        Path root = Paths.get(args[0]).toAbsolutePath().normalize();
        ROOT = root;
        boolean nextOnly = Arrays.asList(args).contains("--next");
        boolean mapOnly = Arrays.asList(args).contains("--map");
        if (!Files.isDirectory(root)) {
            System.err.println("not a directory: " + root);
            System.exit(2);
        }

        List<Path> files;
        try (var stream = Files.walk(root)) {
            files = stream
                .filter(Files::isRegularFile)
                .filter(p -> p.toString().endsWith(".java"))
                .filter(p -> !p.toString().contains("/.git/"))
                .filter(p -> !p.toString().contains("/build/"))
                .filter(p -> !p.toString().contains("/target/"))
                .sorted()
                .collect(Collectors.toList());
        }
        if (files.isEmpty()) {
            System.err.println("no .java files under " + root);
            System.exit(2);
        }

        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        if (compiler == null) {
            System.err.println("javac が無い (JDK で実行してください)");
            System.exit(2);
        }

        DiagnosticCollector<JavaFileObject> diagnostics = new DiagnosticCollector<>();
        StandardJavaFileManager fm = compiler.getStandardFileManager(
            diagnostics, null, StandardCharsets.UTF_8);
        Iterable<? extends JavaFileObject> units = fm.getJavaFileObjectsFromPaths(files);

        List<String> options = List.of("-proc:none", "-Xlint:all");
        JavacTask task = (JavacTask) compiler.getTask(
            null, fm, diagnostics, options, null, units);

        List<CompilationUnitTree> cus = new ArrayList<>();
        for (CompilationUnitTree cu : task.parse()) cus.add(cu);

        boolean analyzed = true;
        try {
            task.analyze();
        } catch (RuntimeException | IOException ex) {
            analyzed = false;
            System.err.println("javac analyze failed: " + ex.getMessage());
        }

        Trees trees = Trees.instance(task);
        Elements elements = task.getElements();
        Types types = task.getTypes();

        Collector collector = new Collector(trees, elements, types);
        for (CompilationUnitTree cu : cus) collector.scan(cu, null);

        List<MethodInfo> methods = new ArrayList<>(collector.methods.values());

        // override 元 (interface / super class) を引く。
        for (MethodInfo mi : methods) {
            mi.contracts.addAll(overridden(mi.owner, mi.element, elements, types));
        }

        // 呼び出しを、実メソッドとその契約 (interface 経由の呼び出し) の両方へ配る。
        // 契約呼び出しは、呼び出し元が同じ契約の別実装を new しているなら
        // その実装には配らない (checkAlignment の呼び出しを Cohesion に混ぜない)。
        Map<ExecutableElement, List<MethodInfo>> byContract = new HashMap<>();
        for (MethodInfo mi : methods) {
            for (ExecutableElement contract : mi.contracts) {
                byContract.computeIfAbsent(contract, k -> new ArrayList<>()).add(mi);
            }
        }
        for (Call c : collector.calls) {
            for (MethodInfo mi : methods) {
                if (mi.element.equals(c.target)) {
                    Call direct = copyCall(c);
                    direct.contract = false;
                    mi.calls.add(direct);
                    continue;
                }
                if (!mi.contracts.contains(c.target)) continue;

                boolean thisNew = c.callerSource != null
                    && c.callerSource.contains("new " + mi.owner.getSimpleName() + "(");
                boolean otherNew = false;
                for (MethodInfo sibling : byContract.getOrDefault(c.target, List.of())) {
                    if (sibling == mi || c.callerSource == null) continue;
                    if (c.callerSource.contains("new " + sibling.owner.getSimpleName() + "(")) {
                        otherNew = true;
                        break;
                    }
                }
                if (!thisNew && otherNew) continue;

                Call indirect = copyCall(c);
                // 呼び出し元がこの実装を new しているなら、動的型はほぼ確定。
                indirect.contract = !thisNew;
                mi.calls.add(indirect);
            }
        }

        // テスト期待値を、そのテストメソッド内で new している型のメソッドへ配る。
        // スタブ判定より先に配ることで、「期待値は非中立なのに本体は 0 を返す」
        // という材料を判定側が使える。
        for (MethodInfo mi : methods) {
            for (Expect e : collector.expects) {
                if (e.callerSource == null) continue;
                if (!actualMentions(e.actual, mi.element.getSimpleName().toString())) continue;
                if (!e.callerSource.contains("new " + mi.owner.getSimpleName() + "(")) continue;
                if (mi.expects.size() >= 8) break;
                mi.expects.add(e);
            }
        }

        // スタブ判定。マーカーが無くても、契約・本体の形・期待値から決める。
        for (MethodInfo mi : methods) {
            mi.stub = stubScore(mi);
            if (mi.stub > 0) mi.marker = markerOf(mi.bodySource);
        }

        List<MethodInfo> stubs = methods.stream()
            .filter(m -> m.stub > 0)
            .sorted((a, b) -> {
                int c = Double.compare(b.stub, a.stub);
                if (c != 0) return c;
                c = Integer.compare(b.calls.size(), a.calls.size());
                if (c != 0) return c;
                return a.qualifiedName().compareTo(b.qualifiedName());
            })
            .collect(Collectors.toList());

        List<Diagnostic<? extends JavaFileObject>> errors = diagnostics.getDiagnostics().stream()
            .filter(d -> d.getKind() == Diagnostic.Kind.ERROR)
            .collect(Collectors.toList());

        printHeader(root, files.size(), methods.size(), stubs.size(), analyzed);
        printDiagnostics(diagnostics, root);

        if (nextOnly) {
            printNext(stubs);
        } else if (mapOnly) {
            printFixMap(stubs, trees);
        } else {
            if (!stubs.isEmpty()) printFixMap(stubs, trees);
            printPlan(stubs, errors, root, elements, types, trees);
            for (MethodInfo mi : stubs) printStub(mi, elements, types, trees);
            printVerify(stubs, errors);
        }

        fm.close();
    }

    // ------------------------------------------------------------------
    // 走査
    // ------------------------------------------------------------------

    static final class Collector extends TreePathScanner<Void, Void> {
        final Trees trees;
        final Elements elements;
        final Types types;
        final Map<ExecutableElement, MethodInfo> methods = new LinkedHashMap<>();
        final List<Call> calls = new ArrayList<>();
        final List<Expect> expects = new ArrayList<>();
        final Map<CompilationUnitTree, String> sourceCache = new HashMap<>();

        Collector(Trees trees, Elements elements, Types types) {
            this.trees = trees;
            this.elements = elements;
            this.types = types;
        }

        String source(CompilationUnitTree cu) {
            return sourceCache.computeIfAbsent(cu, c -> {
                try {
                    return c.getSourceFile().getCharContent(true).toString();
                } catch (IOException e) {
                    return "";
                }
            });
        }

        @Override
        public Void visitMethod(MethodTree tree, Void unused) {
            Element e = trees.getElement(getCurrentPath());
            if (e instanceof ExecutableElement ee && ee.getEnclosingElement() instanceof TypeElement owner) {
                MethodInfo mi = new MethodInfo();
                mi.element = ee;
                mi.tree = tree;
                mi.cu = getCurrentPath().getCompilationUnit();
                mi.owner = owner;
                mi.file = sourcePath(mi.cu, getCurrentPath());
                SourcePositions sp = trees.getSourcePositions();
                mi.start = sp.getStartPosition(mi.cu, tree);
                mi.end = sp.getEndPosition(mi.cu, tree);
                LineMap lm = mi.cu.getLineMap();
                // @Override ではなく、戻り値 (無ければ修飾子) の行を「書く場所」にする。
                long declStart = mi.start;
                if (tree.getReturnType() != null) {
                    long rs = sp.getStartPosition(mi.cu, tree.getReturnType());
                    if (rs >= 0) declStart = rs;
                } else if (tree.getModifiers() != null) {
                    long ms = sp.getStartPosition(mi.cu, tree.getModifiers());
                    if (ms >= 0) declStart = ms;
                }
                mi.line = (int) lm.getLineNumber(declStart);
                mi.column = (int) lm.getColumnNumber(declStart);
                mi.startLine = (int) lm.getLineNumber(mi.start);
                String src = source(mi.cu);
                if (mi.start >= 0 && mi.end >= mi.start && mi.end <= src.length())
                    mi.source = src.substring((int) mi.start, (int) mi.end);
                if (tree.getBody() != null) {
                    long bs = sp.getStartPosition(mi.cu, tree.getBody());
                    long be = sp.getEndPosition(mi.cu, tree.getBody());
                    if (bs >= 0 && be >= bs && be <= src.length()) {
                        mi.bodySource = src.substring((int) bs, (int) be);
                        mi.bodyLine = (int) lm.getLineNumber(bs);
                        mi.bodyEndLine = (int) lm.getLineNumber(be);
                    }
                }
                methods.put(ee, mi);
            }
            return super.visitMethod(tree, unused);
        }

        @Override
        public Void visitMethodInvocation(MethodInvocationTree tree, Void unused) {
            Element e = trees.getElement(getCurrentPath());
            if (e instanceof ExecutableElement ee) {
                MethodTree callerTree = enclosingMethod(getCurrentPath());
                String caller = "?";
                if (callerTree != null) {
                    Element ce = trees.getElement(new TreePath(getCurrentPath(), callerTree));
                    if (ce instanceof ExecutableElement cee
                        && cee.getEnclosingElement() instanceof TypeElement cowner) {
                        caller = cowner.getQualifiedName() + "." + cee.getSimpleName();
                    }
                }
                Call c = new Call();
                c.target = ee;
                c.caller = caller;
                c.where = location(getCurrentPath());
                c.text = textOf(getCurrentPath());
                if (callerTree != null) {
                    c.callerSource = methodSource(getCurrentPath().getCompilationUnit(), callerTree);
                }
                calls.add(c);
            }

            // expect(label, expected, actual) 形状。名前ではなく引数の形で拾う。
            if (tree.getArguments().size() == 3
                && tree.getArguments().get(0) instanceof LiteralTree lit
                && lit.getValue() instanceof String label) {
                Expect ex = new Expect();
                ex.label = label;
                ex.expected = textOf(getCurrentPath(), tree.getArguments().get(1));
                ex.actual = textOf(getCurrentPath(), tree.getArguments().get(2));
                ex.where = location(getCurrentPath());
                MethodTree callerTree = enclosingMethod(getCurrentPath());
                if (callerTree != null) {
                    Element ce = trees.getElement(new TreePath(getCurrentPath(), callerTree));
                    if (ce instanceof ExecutableElement cee
                        && cee.getEnclosingElement() instanceof TypeElement cowner) {
                        ex.caller = cowner.getQualifiedName() + "." + cee.getSimpleName();
                    }
                    SourcePositions sp = trees.getSourcePositions();
                    CompilationUnitTree cu = getCurrentPath().getCompilationUnit();
                    long s = sp.getStartPosition(cu, callerTree);
                    long t = sp.getEndPosition(cu, callerTree);
                    String src = source(cu);
                    if (s >= 0 && t <= src.length()) ex.callerSource = src.substring((int) s, (int) t);
                }
                expects.add(ex);
            }
            return super.visitMethodInvocation(tree, unused);
        }

        String methodSource(CompilationUnitTree cu, MethodTree tree) {
            SourcePositions sp = trees.getSourcePositions();
            long s = sp.getStartPosition(cu, tree);
            long t = sp.getEndPosition(cu, tree);
            String src = source(cu);
            return (s >= 0 && t >= s && t <= src.length()) ? src.substring((int) s, (int) t) : null;
        }

        MethodTree enclosingMethod(TreePath path) {
            TreePath p = path;
            while (p != null) {
                if (p.getLeaf() instanceof MethodTree mt) return mt;
                p = p.getParentPath();
            }
            return null;
        }

        String location(TreePath path) {
            CompilationUnitTree cu = path.getCompilationUnit();
            SourcePositions sp = trees.getSourcePositions();
            long pos = sp.getStartPosition(cu, path.getLeaf());
            LineMap lm = cu.getLineMap();
            return sourcePath(cu, path) + ":" + lm.getLineNumber(pos) + ":" + lm.getColumnNumber(pos);
        }

        String textOf(TreePath path) {
            return textOf(path, path.getLeaf());
        }

        String textOf(TreePath path, Tree tree) {
            CompilationUnitTree cu = path.getCompilationUnit();
            SourcePositions sp = trees.getSourcePositions();
            long s = sp.getStartPosition(cu, tree);
            long e = sp.getEndPosition(cu, tree);
            String src = source(cu);
            if (s < 0 || e < s || e > src.length()) return tree.toString();
            return src.substring((int) s, (int) e).replace('\n', ' ').trim();
        }

        String sourcePath(CompilationUnitTree cu, TreePath path) {
            JavaFileObject fo = cu.getSourceFile();
            Path p = Paths.get(fo.toUri()).toAbsolutePath().normalize();
            try {
                return ROOT.relativize(p).toString();
            } catch (IllegalArgumentException ex) {
                return p.toString();
            }
        }
    }

    // ------------------------------------------------------------------
    // 契約・スタブ
    // ------------------------------------------------------------------

    static List<ExecutableElement> overridden(TypeElement owner, ExecutableElement method,
                                              Elements elements, Types types) {
        List<ExecutableElement> out = new ArrayList<>();
        Deque<TypeMirror> queue = new ArrayDeque<>(types.directSupertypes(owner.asType()));
        Set<String> seen = new HashSet<>();
        while (!queue.isEmpty()) {
            TypeMirror tm = queue.poll();
            Element e = types.asElement(tm);
            if (!(e instanceof TypeElement te)) continue;
            if (!seen.add(te.getQualifiedName().toString())) continue;
            for (Element member : te.getEnclosedElements()) {
                if (!(member instanceof ExecutableElement m)) continue;
                if (!m.getSimpleName().contentEquals(method.getSimpleName())) continue;
                if (m.getParameters().size() != method.getParameters().size()) continue;
                if (elements.overrides(method, m, owner)) out.add(m);
            }
            queue.addAll(types.directSupertypes(tm));
        }
        return out;
    }

    static Call copyCall(Call c) {
        Call n = new Call();
        n.target = c.target;
        n.caller = c.caller;
        n.where = c.where;
        n.text = c.text;
        n.callerSource = c.callerSource;
        n.contract = c.contract;
        return n;
    }

    static boolean neutralExpr(String expr) {
        if (expr == null) return false;
        String s = expr.trim();
        // return new ... は材料を使っているので中立にしない。
        return NEUTRAL.matcher(s).matches()
            || s.matches("[A-Za-z_$][\\w$]*\\.(zero|empty|none)\\s*\\(.*\\)");
    }

    static String singleReturn(MethodInfo mi) {
        if (mi.tree.getBody() == null) return null;
        List<? extends StatementTree> stmts = mi.tree.getBody().getStatements();
        if (stmts.size() != 1 || !(stmts.get(0) instanceof ReturnTree ret) || ret.getExpression() == null)
            return null;
        return ret.getExpression().toString();
    }

    static boolean usesParams(MethodInfo mi) {
        String body = mi.bodySource == null ? "" : mi.bodySource;
        for (VariableTree p : mi.tree.getParameters()) {
            if (Pattern.compile("\\b" + Pattern.quote(p.getName().toString()) + "\\b")
                .matcher(body).find()) {
                return true;
            }
        }
        return false;
    }

    static boolean testConflict(MethodInfo mi) {
        for (Expect e : mi.expects) {
            if (!neutralExpr(e.expected)) return true;
        }
        return false;
    }

    // ここが「自動判定」の本体。名前や task 番号ではなく、
    // 契約 (override/interface)・本体の形・パラメータ利用・テスト期待値を見る。
    static double stubScore(MethodInfo mi) {
        if (mi.element.getKind() == ElementKind.CONSTRUCTOR) return 0.0;
        if (mi.tree.getBody() == null) return 0.0;

        String body = mi.bodySource == null ? "" : mi.bodySource;
        boolean marker = MARKER.matcher(body).find();
        boolean unsupported = body.contains("UnsupportedOperationException");
        boolean empty = mi.tree.getBody().getStatements().isEmpty();
        boolean contract = !mi.contracts.isEmpty();
        String ret = singleReturn(mi);
        boolean neutral = neutralExpr(ret);
        boolean used = usesParams(mi);
        boolean conflict = testConflict(mi);

        double score = 0.0;
        if (marker) score = Math.max(score, 0.95);
        if (unsupported) score = Math.max(score, 0.90);
        if (empty) score = Math.max(score, contract ? 0.80 : 0.0);

        if (neutral) {
            if (conflict) {
                // テストは非中立な値を期待しているのに、本体は 0/null を返す。
                score = Math.max(score, 0.85);
            } else if (contract) {
                score = Math.max(score, 0.65);
            } else if (!used) {
                // 引数も読まずに 0 を返す。弱い候補として残す。
                score = Math.max(score, 0.40);
            }
            // 引数を使って 0 を返すなら、計算結果かもしれないので候補にしない。
        }
        return score;
    }

    static String confidence(double score) {
        return score >= 0.80 ? "high" : score >= 0.60 ? "medium" : "low";
    }

    static List<String> evidence(MethodInfo mi, Trees trees) {
        List<String> out = new ArrayList<>();
        if (!mi.marker.isEmpty()) out.add("marker: " + mi.marker);
        if (mi.tree.getBody() == null) return out;

        boolean empty = mi.tree.getBody().getStatements().isEmpty();
        if (empty) out.add("empty body" + (mi.contracts.isEmpty() ? "" : " with contract"));
        if (mi.bodySource != null && mi.bodySource.contains("UnsupportedOperationException"))
            out.add("throws UnsupportedOperationException");

        String ret = singleReturn(mi);
        if (neutralExpr(ret)) out.add("returns neutral value: " + ret);
        if (neutralExpr(ret) && !usesParams(mi)) out.add("does not read any parameter");

        if (!mi.contracts.isEmpty()) {
            ExecutableElement c = mi.contracts.get(0);
            out.add("contract: " + c.getEnclosingElement().getSimpleName() + "."
                + c.getSimpleName() + " " + elementLocation(c, trees));
        }

        for (Expect e : mi.expects) {
            if (!neutralExpr(e.expected)) {
                out.add("test expects " + e.expected + " at " + e.where);
                break;
            }
        }
        if (!mi.calls.isEmpty()) out.add("called from " + mi.calls.size() + " place(s)");
        return out;
    }

    static String markerOf(String body) {
        if (body == null) return "";
        for (String line : body.split("\\R")) {
            if (MARKER.matcher(line).find()) return line.trim();
        }
        return "";
    }

    static boolean actualMentions(String actual, String simpleName) {
        if (actual == null) return false;
        return actual.matches("(?s).*\\b" + Pattern.quote(simpleName) + "\\s*\\(.*");
    }

    // ------------------------------------------------------------------
    // 出力
    // ------------------------------------------------------------------

    static void printHeader(Path root, int files, int methods, int stubs, boolean analyzed) {
        System.out.println("# neodax java review");
        System.out.println("root\t" + root);
        System.out.println("files\t" + files);
        System.out.println("methods\t" + methods);
        System.out.println("stubs\t" + stubs);
        System.out.println("analyzed\t" + analyzed);
        System.out.println();
    }

    static void printDiagnostics(DiagnosticCollector<JavaFileObject> diagnostics, Path root) {
        List<Diagnostic<? extends JavaFileObject>> list = diagnostics.getDiagnostics().stream()
            .filter(d -> d.getKind() == Diagnostic.Kind.ERROR
                      || d.getKind() == Diagnostic.Kind.WARNING
                      || d.getKind() == Diagnostic.Kind.MANDATORY_WARNING)
            .collect(Collectors.toList());
        if (list.isEmpty()) return;
        System.out.println("## javac diagnostics");
        for (var d : list) {
            String where = d.getSource() == null ? "?" :
                root.relativize(Paths.get(d.getSource().toUri()).toAbsolutePath().normalize()) + ":"
                + d.getLineNumber() + ":" + d.getColumnNumber();
            System.out.println(d.getKind() + "\t" + where + "\t" + d.getMessage(Locale.ROOT).replace('\n', ' '));
        }
        System.out.println();
    }

    static void printNext(List<MethodInfo> stubs) {
        System.out.println("## next");
        int n = 1;
        for (MethodInfo mi : stubs.stream().limit(10).collect(Collectors.toList())) {
            System.out.printf("%d\t%.2f\t%s\t%s\t%s:%d\t%s%n",
                n++, mi.stub, confidence(mi.stub), mi.qualifiedName(), mi.file, mi.line, mi.signature());
        }
    }

    static int taskNumber(MethodInfo mi) {
        var m = Pattern.compile("(?i)task\\s*(\\d+)").matcher(mi.marker);
        return m.find() ? Integer.parseInt(m.group(1)) : 999;
    }

    static String fixKey(MethodInfo mi) {
        int n = taskNumber(mi);
        return n == 999 ? "stub" : "task" + n;
    }

    // 「元のコードの名前」と「直す場所」を 1 行で対応させる。
    static void printFixMap(List<MethodInfo> stubs, Trees trees) {
        System.out.println("## fix map");
        System.out.println("key\tname\twhere\tbody\tcontract\texpect\tconf");

        List<MethodInfo> map = new ArrayList<>(stubs);
        map.sort(Comparator.comparingInt(JavaReview::taskNumber)
            .thenComparing(MethodInfo::qualifiedName));

        for (MethodInfo mi : map) {
            String contract = "-";
            if (!mi.contracts.isEmpty()) {
                ExecutableElement c = mi.contracts.get(0);
                contract = c.getEnclosingElement().getSimpleName() + "." + c.getSimpleName()
                    + " " + elementLocation(c, trees);
            }
            String expects = mi.expects.stream()
                .map(e -> e.where)
                .distinct()
                .collect(Collectors.joining(","));
            if (expects.isEmpty()) expects = "-";
            String body = mi.bodyLine > 0 ? mi.bodyLine + "-" + mi.bodyEndLine : "-";
            System.out.println(fixKey(mi) + "\t" + mi.qualifiedName() + "\t"
                + mi.file + ":" + mi.line + ":" + mi.column + "\t" + body
                + "\t" + contract + "\t" + expects + "\t" + confidence(mi.stub));
        }
        System.out.println();
    }

    // 元のコードをそのまま行番号つきで出す。どこを置き換えるかが分かる。
    static void printOriginal(MethodInfo mi) {
        if (mi.source == null || mi.startLine <= 0) return;
        System.out.println("original");
        String[] lines = mi.source.split("\\R", -1);
        int n = mi.startLine;
        int limit = Math.min(lines.length, 30);
        for (int i = 0; i < limit; i++) {
            System.out.printf("  %4d  %s%n", n + i, lines[i]);
        }
        if (lines.length > limit) System.out.println("  ...");
    }

    static String diagWhere(Diagnostic<? extends JavaFileObject> d, Path root) {
        if (d.getSource() == null) return "?";
        Path p = Paths.get(d.getSource().toUri()).toAbsolutePath().normalize();
        String file;
        try { file = root.relativize(p).toString(); }
        catch (IllegalArgumentException ex) { file = p.toString(); }
        return file + ":" + d.getLineNumber() + ":" + d.getColumnNumber();
    }

    static String joinInts(List<Integer> v) {
        return v.stream().map(String::valueOf).collect(Collectors.joining(","));
    }

    static String shortMaterial(MethodInfo mi, Elements elements, Types types, Trees trees) {
        // 1 つの型だけに偏らないよう、型ごとに 3 つまで拾う。
        List<String> out = new ArrayList<>();
        int perType = 0;
        for (String s : material(mi, elements, types, trees)) {
            if (s.startsWith("[")) {
                perType = 0;
                continue;
            }
            if (perType >= 3) continue;
            out.add(s.split("\t")[0]);
            perType++;
            if (out.size() >= 9) break;
        }
        return String.join(", ", out);
    }

    static String expectWhere(MethodInfo mi) {
        String s = mi.expects.stream().map(e -> e.where).distinct().collect(Collectors.joining(","));
        return s.isEmpty() ? "-" : s;
    }

    static String planConfidence(List<MethodInfo> stubs, List<Diagnostic<? extends JavaFileObject>> errors) {
        if (!errors.isEmpty()) return "low";
        if (stubs.isEmpty()) return "high";
        double min = stubs.stream().mapToDouble(m -> m.stub).min().orElse(1.0);
        return confidence(min);
    }

    // neodax の「観測 → 問題 → 計画 → 検証」に合わせた手順。
    // 問題が無ければ step は出さない。
    static void printPlan(List<MethodInfo> stubs,
                          List<Diagnostic<? extends JavaFileObject>> errors,
                          Path root, Elements elements, Types types, Trees trees) {
        System.out.println("## plan");
        if (stubs.isEmpty() && errors.isEmpty()) {
            System.out.println("verdict\tHEALTHY");
            System.out.println("steps\t0");
            System.out.println("(直すべき実装箇所は検出されなかった)");
            System.out.println();
            return;
        }

        System.out.println("verdict\tNEEDS_FIX");
        System.out.println("findings\t" + (errors.size() + stubs.size()));

        int step = 0;
        List<Integer> blockers = new ArrayList<>();
        for (var d : errors) {
            step++;
            blockers.add(step);
            System.out.println("step " + step + "\tfix-compile");
            System.out.println("  where\t" + diagWhere(d, root));
            System.out.println("  action\t" + d.getMessage(Locale.ROOT).replace('\n', ' '));
        }

        List<MethodInfo> ordered = new ArrayList<>(stubs);
        ordered.sort(Comparator.comparingInt(JavaReview::taskNumber)
            .thenComparing((a, b) -> Double.compare(b.stub, a.stub))
            .thenComparing(MethodInfo::qualifiedName));

        List<Integer> verifications = new ArrayList<>();
        for (MethodInfo mi : ordered) {
            step++;
            int impl = step;
            System.out.println("step " + step + "\timplement\t" + fixKey(mi) + " " + mi.qualifiedName());
            System.out.println("  where\t" + mi.file + ":" + mi.line + ":" + mi.column);
            if (mi.bodyLine > 0)
                System.out.println("  body\t" + mi.bodyLine + "-" + mi.bodyEndLine);
            System.out.println("  action\t" + (mi.marker.isEmpty()
                ? "契約と期待値を満たす本体を書く"
                : mi.marker.replaceFirst("^//\\s*", "")));
            if (!mi.contracts.isEmpty()) {
                ExecutableElement c = mi.contracts.get(0);
                System.out.println("  contract\t" + c.getEnclosingElement().getSimpleName()
                    + "." + c.getSimpleName() + " " + elementLocation(c, trees));
            }
            String uses = shortMaterial(mi, elements, types, trees);
            if (!uses.isEmpty()) System.out.println("  uses\t" + uses);
            if (!blockers.isEmpty()) System.out.println("  requires\t" + joinInts(blockers));

            step++;
            verifications.add(step);
            System.out.println("step " + step + "\tverify\t" + fixKey(mi));
            System.out.println("  requires\t" + impl);
            String expects = expectWhere(mi);
            if (!"-".equals(expects)) {
                System.out.println("  action\t" + expects + " の期待値を確認");
            } else {
                System.out.println("  action\tこのメソッドがスタブ判定から外れることを確認");
            }
        }

        step++;
        System.out.println("step " + step + "\tre-review");
        List<Integer> req = verifications.isEmpty() ? blockers : verifications;
        if (!req.isEmpty()) System.out.println("  requires\t" + joinInts(req));
        System.out.println("  action\tneodax-java-review " + root);
        System.out.println("  expect\tstubs=0 errors=0");
        System.out.println("steps\t" + step);
        System.out.println("confidence\t" + planConfidence(stubs, errors));
        System.out.println();
    }

    static void printVerify(List<MethodInfo> stubs,
                            List<Diagnostic<? extends JavaFileObject>> errors) {
        System.out.println("## verify");
        System.out.println("result\t" + ((stubs.isEmpty() && errors.isEmpty()) ? "HEALTHY" : "NEEDS_FIX"));
        System.out.println("expected\tstubs=0 errors=0");
        System.out.println("confidence\t" + planConfidence(stubs, errors));
        if (stubs.isEmpty() && errors.isEmpty())
            System.out.println("(バグは検出されなかった。直す手順は出さない)");
    }

    static void printStub(MethodInfo mi, Elements elements, Types types, Trees trees) {
        System.out.println("## " + mi.qualifiedName());
        System.out.println("where\t" + mi.file + ":" + mi.line + ":" + mi.column);
        System.out.println("stub\t" + String.format("%.2f", mi.stub));
        System.out.println("confidence\t" + confidence(mi.stub));
        System.out.println("signature\t" + mi.signature());
        if (!mi.marker.isEmpty()) System.out.println("marker\t" + mi.marker);
        for (String why : evidence(mi, trees)) System.out.println("why\t" + why);

        for (ExecutableElement c : mi.contracts) {
            String loc = elementLocation(c, trees);
            String doc = elements.getDocComment(c);
            System.out.println("contract\t" + c.getEnclosingElement() + "." + c.getSimpleName()
                + "(" + c.getParameters().size() + " args)\t" + loc
                + (doc == null ? "" : "\t" + doc.replace('\n', ' ')));
        }

        for (Call c : mi.calls.stream().limit(12).collect(Collectors.toList())) {
            System.out.println((c.contract ? "call(contract)" : "call")
                + "\t" + c.caller + "\t" + c.where + "\t" + c.text);
        }

        for (Expect e : mi.expects) {
            System.out.println("expect\t" + e.where + "\t" + e.label
                + "\texpected=" + e.expected + "\tactual=" + e.actual);
        }

        System.out.println("material");
        for (String m : material(mi, elements, types, trees)) System.out.println("  " + m);

        printOriginal(mi);

        System.out.println("skeleton");
        String ret = mi.tree.getReturnType() == null ? "void" : mi.tree.getReturnType().toString();
        String params = mi.tree.getParameters().stream()
            .map(p -> p.getType() + " " + p.getName())
            .collect(Collectors.joining(", "));
        if (!mi.contracts.isEmpty()) System.out.println("  @Override");
        String mods = mi.tree.getModifiers().getFlags().stream()
            .filter(f -> f == Modifier.PUBLIC || f == Modifier.PROTECTED || f == Modifier.PRIVATE)
            .map(f -> f.toString().toLowerCase(Locale.ROOT) + " ")
            .collect(Collectors.joining());
        System.out.println("  " + mods + ret + " " + mi.element.getSimpleName()
            + "(" + params + ") {");
        String todo = mi.marker.isEmpty() ? "未実装"
            : mi.marker.replaceFirst("^//\\s*", "").replaceFirst("(?i)^todo:?\\s*", "");
        System.out.println("      // TODO: " + todo);
        System.out.println("  }");
        System.out.println();
    }

    static String elementLocation(ExecutableElement e, Trees trees) {
        TreePath p = trees.getPath(e);
        if (p == null) return "(no source)";
        return elementLocationAny(e, trees);
    }

    static List<String> material(MethodInfo mi, Elements elements, Types types, Trees trees) {
        Set<TypeElement> focus = new LinkedHashSet<>();
        // 戻り値と引数の型を先に。自分のクラスは最後 (材料として弱い)。
        addTypeMirror(focus, mi.element.getReturnType(), types);
        for (VariableElement p : mi.element.getParameters()) {
            addTypeMirror(focus, p.asType(), types);
        }
        addType(focus, mi.owner);

        List<String> out = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (TypeElement te : focus) {
            List<Element> members = new ArrayList<>(elements.getAllMembers(te));
            members.sort((a, b) -> {
                String an = a.getSimpleName().toString();
                String bn = b.getSimpleName().toString();
                String want = mi.tree.getReturnType() == null ? "" : simpleType(mi.tree.getReturnType().toString());
                boolean am = a instanceof ExecutableElement ae
                    && simpleType(ae.getReturnType().toString()).equals(want);
                boolean bm = b instanceof ExecutableElement be
                    && simpleType(be.getReturnType().toString()).equals(want);
                if (am != bm) return am ? -1 : 1;
                return an.compareTo(bn);
            });
            List<String> lines = new ArrayList<>();
            for (Element m : members) {
                if (m.equals(mi.element)) continue;
                // 自分のクラスの private は、このメソッドから使える。
                if (m.getModifiers().contains(Modifier.PRIVATE) && !te.equals(mi.owner)) continue;
                if (m.getEnclosingElement() instanceof TypeElement owner
                    && owner.getQualifiedName().toString().startsWith("java.")) continue;
                String loc = elementLocationAny(m, trees);
                if (loc == null) continue;
                String line;
                if (m instanceof ExecutableElement me) {
                    if (me.getSimpleName().contentEquals("<init>")) continue;
                    String params = me.getParameters().stream()
                        .map(p -> simpleType(p.asType().toString()))
                        .collect(Collectors.joining(", "));
                    line = te.getSimpleName() + "." + me.getSimpleName() + "(" + params + ") -> "
                        + simpleType(me.getReturnType().toString()) + "\t" + loc;
                } else if (m instanceof VariableElement ve) {
                    line = te.getSimpleName() + "." + ve.getSimpleName() + " : "
                        + simpleType(ve.asType().toString()) + "\t" + loc;
                } else {
                    continue;
                }
                if (seen.add(line)) {
                    lines.add(line);
                    if (lines.size() >= 8) break;
                }
            }
            if (!lines.isEmpty()) {
                out.add("[" + te.getSimpleName() + "]");
                out.addAll(lines);
            }
        }
        if (out.isEmpty()) out.add("(このリポジトリ内に材料が見つからない)");
        return out;
    }

    static void addType(Set<TypeElement> focus, TypeElement te) {
        if (te != null) focus.add(te);
    }

    static void addTypeMirror(Set<TypeElement> focus, TypeMirror tm, Types types) {
        if (tm == null) return;
        if (tm instanceof DeclaredType dt) {
            Element e = dt.asElement();
            if (e instanceof TypeElement te) focus.add(te);
            for (TypeMirror arg : dt.getTypeArguments()) addTypeMirror(focus, arg, types);
        } else if (tm instanceof ArrayType at) {
            addTypeMirror(focus, at.getComponentType(), types);
        }
    }

    static String elementLocationAny(Element e, Trees trees) {
        TreePath p = trees.getPath(e);
        if (p == null) return null;
        CompilationUnitTree cu = p.getCompilationUnit();
        long pos = trees.getSourcePositions().getStartPosition(cu, p.getLeaf());
        if (pos < 0) return null;
        LineMap lm = cu.getLineMap();
        String file = Paths.get(cu.getSourceFile().toUri()).toAbsolutePath().normalize().toString();
        try {
            file = ROOT.relativize(Paths.get(file)).toString();
        } catch (IllegalArgumentException ignored) {
        }
        return file + ":" + lm.getLineNumber(pos) + ":" + lm.getColumnNumber(pos);
    }

    static String simpleType(String s) {
        return s.replaceAll("\\b(?:[a-zA-Z_$][\\w$]*\\.)+([A-Z][\\w$]*)", "$1");
    }
}
