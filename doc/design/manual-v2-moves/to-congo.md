
<!-- from: mod-packages.html#package (Lane D), the tool side | suggested home: the separate Congo document Jon wants | why: the manifest, the commands, the build folder and the sample session are the build tool, not the language; mod-packages keeps what a package is, what it shows, how it is compiled apart, and one-direction dependencies. Verbatim, the page as it stood at a74d7982a, lines 31-100 (the package layout, the manifest, the commands, the session): -->
	<p>A <b>package</b> is one top-level module and the modules inside it, built on
		its own and shared as a unit. It is a folder holding a manifest,
		<span class="pre">congo.toml</span>, and the source under
		<span class="pre">src</span>, whose root module's designated file is named
		for the package:</p>
<pre>
hello/
    congo.toml
    src/hello.cone
    tests/
</pre>
<pre>
[package]
name = "hello"
version = "0.1.0"
output = "executable"
</pre>
	<p>The manifest gives the package's name, its version
		(<span class="pre">MAJOR.MINOR.PATCH</span>), and whether it builds into an
		<span class="pre">executable</span> or a <span class="pre">library</span>.
		It lists no dependencies: the package's <span class="pre">import</span>
		statements are its dependencies. Under <span class="pre">src</span> the
		<a href="mod-namespaces.html#dcl">module rules</a> apply unchanged, with
		<span class="pre">src/hello.cone</span> as the root's designated file.</p>
	<p>A package may also have a <span class="pre">gpu</span> folder beside
		<span class="pre">src</span>, holding the part of it that runs on a GPU as well:
		more submodules of the root, each a file opening with its own
		<span class="pre">mod</span> or a folder holding its designated file, which
		<span class="pre">src</span> reaches by name as it reaches any submodule, and which
		may not use <span class="pre">src</span> in return. The
		<a href="conc-gpu.html#gpufolder">GPU Compute</a> page has the rules.</p>
	<p>The build tool, <b>Congo</b>, builds packages. <span class="pre">congo new
		hello</span> makes the folder above; <span class="pre">congo run</span>,
		from anywhere inside it, builds it and runs it;
		<span class="pre">congo build</span> only builds, and
		<span class="pre">--release</span> optimises. A single file with no
		manifest is run with <span class="pre">congo run hello.cone</span>.
		Congo finds each package a program imports by its name, in a list of
		package folders, compiles each one on its own, and links them. A package
		is imported through its <a href="safe-c.html#includefile">include
		file</a>, which compiling the package generates from its source:
		<span class="pre">build/debug/hello.cone</span>, beside its object. Congo
		compiles every package before what imports it, and compiles each importer
		against that generated file; nothing is written by hand. The <a href="library.html">packages that come with Cone</a>,
		<span class="pre">core</span>, <span class="pre">stdio</span>,
		<span class="pre">libc</span> and <span class="pre">posix</span>, are
		packages of this shape, the last two C packages (below). Imports between packages may not loop: a package
		never imports, directly or through others, a package that imports it.
		Nor may the modules inside one (<a href="#dag">below</a>). Congo checks
		both before it compiles anything.</p>
	<!-- Moved here 15 Oct 2026 from "A first program". -->
	<p>A session making and running a package looks like this:</p>
<pre>
&gt; congo new hello
     Created executable package hello (hello)
&gt; cd hello
&gt; congo run
   Compiling libc v0.1.0 (C:\src\cone\packages\libc)
   Compiling core v0.1.0 (C:\src\cone\packages\core)
   Compiling stdio v0.1.0 (C:\src\cone\packages\stdio)
   Compiling hello v0.1.0 (C:\work\hello)
     Linking build\debug\hello.exe
    Finished debug build\debug\hello.exe
     Running build\debug\hello.exe
Hello, world!
</pre>
	<p>What a build writes goes in the package's <span class="pre">build/debug</span> folder
		(<span class="pre">build/release</span> for <span class="pre">--release</span>). Arguments after
		<span class="pre">--</span> go to the program: <span class="pre">congo run -- a b</span>.
		<span class="pre">congo test</span> runs a package's tests (<a href="lib-test.html">Testing</a>).</p>

<!-- from: mod-packages.html#cpackage and #linksections (Lane D) | suggested home: Congo document, "C packages" | why: the [link] table and its per-system sections are manifest keys; mod-packages keeps one sentence on what a C package is, with id="cpackage" and id="linksections" kept on it. Verbatim, lines 101-136: -->
	<p id="cpackage">A <b>C package</b> wraps a C library. Its source is a
		<a href="safe-c.html#cnames">C-named</a> module, <span class="pre">src/<i>name</i>.cone</span>,
		declaring what the library defines, and its include file is generated from
		it as any package's is. Its manifest names the C library under
		<span class="pre">[link]</span>, and Congo links that library into every
		program that imports the package:</p>
<pre>
[package]
name = "winstr"
version = "0.1.0"
output = "library"

[link]
libraries = ["shlwapi"]      # shlwapi.lib on Windows, -lshlwapi elsewhere
paths = ["clib"]             # folders to look in first, relative to the package
</pre>
	<p>A program imports it by name, <span class="pre">import winstr;</span>, with
		no C declarations of its own. Any package may have a
		<span class="pre">[link]</span> table, a program's included.</p>
	<p id="linksections">What differs by system goes in a section of
		<span class="pre">[link]</span> for that system:
		<span class="pre">[link.windows]</span>, <span class="pre">[link.linux]</span> and
		<span class="pre">[link.macos]</span>, each with the same keys
		(<span class="pre">libraries</span>, <span class="pre">paths</span> and
		<span class="pre">runtime</span>). The plain <span class="pre">[link]</span> applies on
		every system, and the section for the system being built for adds to it, each list
		the plain one's entries and then the section's; a section for another system adds
		nothing. A package with nothing in common writes no plain
		<span class="pre">[link]</span>:</p>
<pre>
[link.windows]
libraries = ["libssl", "libcrypto", "crypt32"]

[link.linux]
libraries = ["ssl", "crypto"]
</pre>

<!-- from: mod-packages.html#build (Lane D) | suggested home: Congo document, "What a build does" (the .conebuild format) | why: the build description is the file Congo hands the compiler; mod-packages keeps, in prose, the language rules that came in its list (a library is named in its symbols, exports what importers need, a generic is made by every package that uses it, the include file). Verbatim, lines 138-218: -->
	<h2 id="build">A package's build description</h2>
	<p>A package can be built on its own. The build tool, Congo, reads each file's
		<span class="pre">mod</span> line and imports, and hands the compiler a
		<b>build description</b>: a file ending in
		<span class="pre">.conebuild</span> that names each module of the package,
		lists its files, and says where each of its imports is found. The compiler
		is given that file in place of a source file:</p>
<pre>
build: debug
output: library
import core: "core.cone"
import stdio: "stdio.cone"
geometry: {
    "../../src/geometry.cone"
    "../../src/shapes.cone"
    import stdio: "stdio.cone"
    scaling: {
        "../../src/scaling/scaling.cone"
    }
}
</pre>
	<ul>
		<li><b>The settings come first.</b> <span class="pre">build</span> is
			<span class="pre">debug</span> or <span class="pre">release</span>, and
			<span class="pre">output</span> is <span class="pre">executable</span> (the
			default) or <span class="pre">library</span>.</li>
		<li><b>Then the package lines</b>, one
			<span class="pre">import name: "path"</span> for every package the
			package depends on, directly or through another package, each naming
			that package's include file. They are where an include file's own
			imports are found: an include file is a module file like any other,
			so it may import, and a package whose functions take or return another
			package's types imports that package in its include file. They do not
			add to what the package's own modules import. The line for
			<span class="pre">core</span>, where there is one, is also where the
			prelude every module imports is loaded from. Congo writes the
			description into the build folder, beside the include files the earlier
			compiles generated.</li>
		<li><b>Then the package's module</b>, a name and a braced body. In it, a quoted
			path is a file of the module, <span class="pre">name: { ... }</span> is a
			module inside it, and <span class="pre">import name: "path"</span> says
			where this module's <span class="pre">import name</span> is found.
			A relative path is relative to the description's own folder, and is
			written with <span class="pre">/</span>, since the description is read as
			Cone source and a backslash begins an escape.</li>
		<li><b>The description names each module</b>, and a
			<span class="pre">mod</span> line opening any of a module's files must
			restate that name. Only the first file listed may declare the module, as
			only a designated file may; a module inside the package is private to it
			unless its own <span class="pre">mod</span> line says
			<span class="pre">pub</span>, exactly as a submodule is.</li>
		<li><b>Nothing is searched for.</b> A file the description does not list is
			not compiled, and an import is found only where its
			<a href="mod-namespaces.html#described">import line</a> says.</li>
		<li><b>A library's module names its symbols.</b> Built with
			<span class="pre">output: library</span>, every declaration of the package
			is spelled after its module — a function <span class="pre">area</span> as
			<span class="pre">geometry.area</span> — as a
			program importing <span class="pre">geometry</span> spells it. A program's
			module is not, so its <span class="pre">main</span> is the entry point the
			linker expects.</li>
		<li><b>A library exports what its importers need.</b> Its public functions and
			globals, and the public functions of its public types, are visible to
			the linker, so a program compiled against the library's
			<a href="mod-namespaces.html#described">include file</a> links with the
			library's own object. So is a private function, global or type that an
			<span class="pre">inline</span>, generic or macro body, or a trait's
			default, names: the importer builds its own copy of that body, and the
			copy calls what it names. Everything else the package defines stays
			private to its object.</li>
		<li><b>A generic is made by every package that uses it.</b> Each object
			holds the versions of a generic that its own code uses, the library's
			and the program's alike, as it holds the table a type's
			<a href="mod-substitution.html#traits">trait</a> methods are reached through. Where two
			objects hold the same one, linking keeps a single copy.</li>
	</ul>
	<p>The include file declares each function, method and global the library
		defines with <a href="safe-c.html#includefile"><span class="pre">extern</span></a>
		and no body. Its module is Cone-named like the library's, so each
		declaration is spelled as the library defines it; a generic is written
		whole.</p>
