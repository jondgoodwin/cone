# Cone Language Website
This repository contains the server source code for the [Cone Language Website][conesite],
which:

* Summarizes the Cone language features
* Provides an online playground for running Cone programs
* Offers reference documentation for the Cone language
* Lists available Cone libraries
* Describes Cone ecosystem tools

Using the playground, a Cone programmer can use a web browser to create and run Cone programs,
thereby avoiding the need to first install the [Cone compiler and toolchain][cone].
It also supports the generation of assembler output, LLVM IR, or Github-hosted code gists.

## Technology Dependencies

The website's server code depends on Ruby, Passenger/Rack and the Sinatra framework for url routing,
automated assembly of pages using templates, and server-based Cone compilation activities.

The playground's front-end makes use of the [Ace High-Performance Code Editor for the Web][ace] for program editing.
The other .html, .css, and .js files are adapted from the Pony playground, which in turn
adapted them from an [earlier version of the Rust playground][rustplay].

## License

These files and tools are distributed under the terms of multiple licenses,
depending on the original source for the code. 
See LICENSE for details.

[conesite]: http://cone.jondgoodwin.com
[cone]: http://github.com/jondgoodwin/cone
[ace]: https://ace.c9.io/
[rustplay]: https://github.com/rust-lang/rust-playpen
