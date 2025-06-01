# lc3vm
[LC-3](https://en.wikipedia.org/wiki/Little_Computer_3) virtual machine and interactive debugger

<img width="1026" alt="image" src="https://github.com/jackwherry/lc3vm/assets/17338790/4bb41f14-4464-416e-82af-05cb338e730c">


This project emulates the LC-3 instruction set. In addition, it lets you single-step through instructions, inspect memory at any location, pause the running emulator with Ctrl-C, and more. When you step forward in the single-step mode, you'll see how each instruction was interpreted and what changes it made to the memory and registers. I used the linenoise library to support command editing with history, so you won't be frustrated by ^[[A everywhere. 

## Building
Compile this project as you would any other C project on your system. A Makefile is included, but it may not work for you. If you're on Windows, you'll likely need MinGW or similar.

## Acknowledgements
Opcode explanations and implementations courtesy of [this blog post](https://www.jmeiners.com/lc3-vm/).
