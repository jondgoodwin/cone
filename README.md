# Cone Programming Language
Cone is a statically-type, object-oriented programming language. 
It is optimized for building interactive, web-based, high performance 3-D worlds.

Planned features:

- Seamlessly marries content and behavior
- Modules, Interfaces, Generics and multiple inheritance Classes
- Lightweight concurrency
- Memory, type, and concurrency safe, via robust type systems
- [Gradual memory management][gmm], supporting multiple memory management strategies

## Documentation

Language documentation and examples are TBD.

This [website][web3d] offers additional information about the Web3D vision.

## Building (Linux)

To build the library:

	cmake

1. By default, a local 'obj' directory is created, and all 'src' compiled into it.
2. A local 'lib' directory is created, and 'w3d_data.a' is created.
3. The test program is created in 'test' from its compiled source and library.

To run the test program:

	test/test_data

## Building (Windows)

A Visual C++ solutions file can be created using the project files. 
The generated object, library, and executable files are created relative to the location of the 
solutions file.

## License

The Cone programming language compiler is distributed under the terms of the MIT license. 
See LICENSE and COPYRIGHT for details.

[web3d]: http://web3d.jondgoodwin.com
[gmm]: http://jondgoodwin.com/pling/gmm.pdf
