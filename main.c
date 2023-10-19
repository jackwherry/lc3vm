#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
// unix only
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

#include "linenoise.h"

struct termios original_tio;

void disable_input_buffering(void) {
	tcgetattr(STDIN_FILENO, &original_tio);
	struct termios new_tio = original_tio;
	new_tio.c_lflag &= ~ICANON & ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering(void) {
	tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key(void) {
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(STDIN_FILENO, &readfds);

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

// machine state
enum {
	S_OFF = 0,
	S_STEP, // single-step/debugging mode
	S_TURBO // full speed
};

int state = S_STEP;

void handle_interrupt(int signal) {
	(void) signal; // we're intentionally handling all signals the same way
	state--;
	if (state == 0) {
		// this code won't run currently because linenoise handles it for us
		restore_input_buffering();
		printf("\n");
		exit(-2);
	} else {
		printf("Dropped into single-step mode. Press ^C again to quit.\n");
	}
}

// memory
#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX];

// registers
enum {
	R_R0 = 0,
	R_R1,
	R_R2,
	R_R3,
	R_R4,
	R_R5,
	R_R6,
	R_R7,
	R_PC,
	R_COND,
	R_COUNT
};

uint16_t reg[R_COUNT];

// opcodes
enum {
	OP_BR = 0,	// branch
	OP_ADD,		// add
	OP_LD,		// load
	OP_ST,		// store
	OP_JSR,		// jump register
	OP_AND,		// bitwise and
	OP_LDR,		// load register
	OP_STR,		// store register
	OP_RTI,		// unused
	OP_NOT,		// bitwise not
	OP_LDI,		// load indirect
	OP_STI,		// store indirect
	OP_JMP,		// jump
	OP_RES,		// reserved (unused)
	OP_LEA,		// load effective address
	OP_TRAP		// execute trap
};

// condition flags
enum {
	FL_POS = 1 << 0, // P
	FL_ZRO = 1 << 1, // Z
	FL_NEG = 1 << 2  // N
};

// trap codes
enum {
	TRAP_GETC = 0x20,	// get character from keyboard, don't echo to terminal
	TRAP_OUT = 0x21,	// output a character
	TRAP_PUTS = 0x22,	// output a word string
	TRAP_IN = 0x23,		// get character from keyboard, do echo to terminal
	TRAP_PUTSP = 0x24,	// output a byte string
	TRAP_HALT = 0x25	// halt the machine
};

// memory-mapped registers
enum {
	MR_KBSR = 0xFE00, // keyboard status
	MR_KBDR = 0xFE02  // keyboard data
};

uint16_t sign_extend(uint16_t x, int bit_count) {
	if ((x >> (bit_count - 1)) & 1) {
		x |= (0xFFFF << bit_count);
	}
	return x;
}

uint16_t swap16(uint16_t x) {
	return (x << 8) | (x >> 8);
}

void mem_write(uint16_t address, uint16_t value) {
	memory[address] = value;
}

uint16_t mem_read(uint16_t address) {
	// handle memory-mapped registers
	if (address == MR_KBSR) {
		if (check_key()) {
			memory[MR_KBSR] = (1 << 15);
			memory[MR_KBDR] = getchar();
		} else {
			memory[MR_KBSR] = 0;
		}
	}
	return memory[address];
}

void update_flags(uint16_t r) {
	if (reg[r] == 0) {
		reg[R_COND] = FL_ZRO;
	} else if (reg[r] >> 15) { // if there's a one in the leftmost bit
		reg[R_COND] = FL_NEG;
	} else {
		reg[R_COND] = FL_POS;
	}
	if (state == S_STEP) printf("Set R_COND to 0x%04hX.\n", reg[R_COND]);
}

void read_image_file(FILE* file) {
	// the origin tells us where in memory to put the file
	uint16_t origin;
	fread(&origin, sizeof(origin), 1, file);
	origin = swap16(origin);

	printf("Putting file at 0x%04hX.\n", origin);

	// we know the the maximum file size, so we need only one fread
	uint16_t max_read = MEMORY_MAX - origin;
	uint16_t* p = memory + origin;
	size_t read = fread(p, sizeof(uint16_t), max_read, file);

	// swap endianness
	while (read-- > 0) {
		*p = swap16(*p);
		++p;
	}
}

int read_image(const char* image_path) {
	FILE* file = fopen(image_path, "rb");
	if (!file) { return 0; } // error condition
	read_image_file(file);
	fclose(file);
	return 1; // success
}

int main(int argc, char** argv) {
	signal(SIGINT, handle_interrupt);
	disable_input_buffering();

	if (argc < 2) {
		printf("Usage: lc3vm [image-file1] ...\n");
		exit(2);
	}

	for (int i = 1; i < argc; i++) {
		printf("Loading image file #%d: '%s'...\n", i, argv[i]);
		if (!read_image(argv[i])) {
			printf("Failed to load image: %s.\n", argv[i]);
			exit(1);
		}
	}

	printf("You are in single-step mode. Type (h)elp for help.\n");

	// set the command history available to the user (up arrow to get last command, like the shell)
	if (!linenoiseHistorySetMaxLen(1024)) {
		printf("malloc failed when setting history length, exiting...");
		restore_input_buffering();
		exit(71);
	}

	// exactly one condition flag should be set at a time, so set the zero flag
	reg[R_COND] = FL_ZRO;

	// set the PC to its starting position
	reg[R_PC] = 0x3000;

	while (state) {
		// fetch
		uint16_t instr = mem_read(reg[R_PC]++);
		uint16_t op = instr >> 12; // get first four bits

		// single-step/debugger mode command line
		if (state == S_STEP) {
			restore_input_buffering();
			printf("Fetched instruction from 0x%04hX, containing 0x%04hX.\n", reg[R_PC]-1, instr);

			while (1) {
				// get user command
				char* line = linenoise("(lc3vm) ");

				// linenoise intercepts ^C, so if it receives that, we need to restore and exit
				if (line == NULL) {
					goto end;
				};

				// add command to history
				linenoiseHistoryAdd(line);

				if (!strncmp(line, "h", 1)) {
					printf("lc3vm commands:\n");
					printf("help\t\t\t-- Print this help page.\n");
					printf("continue\t\t-- Continue execution. Get back here with ^C.\n");
					printf("step\t\t\t-- Step forward one instruction.\n");
					printf("memory [addr] [n]\t-- Display n words of memory starting from addr.\n");
					printf("reg\t\t\t-- Display the contents of the registers.\n");

					printf("\nPress ^C or ^D to exit. You can abbreviate commands with their first letters.\n");
				} else if (!strncmp(line, "c", 1)) {
					state++; // move from S_STEP to S_TURBO
					break;
				} else if (!strncmp(line, "s", 1)) {
					break;
				} else if (!strncmp(line, "r", 1)) {
					printf("R0:\t 0x%04hX\n", reg[R_R0]);
					printf("R1:\t 0x%04hX\n", reg[R_R1]);
					printf("R2:\t 0x%04hX\n", reg[R_R2]);
					printf("R3:\t 0x%04hX\n", reg[R_R3]);
					printf("R4:\t 0x%04hX\n", reg[R_R4]);
					printf("R5:\t 0x%04hX\n", reg[R_R5]);
					printf("R6:\t 0x%04hX\n", reg[R_R6]);
					printf("R7:\t 0x%04hX\n", reg[R_R7]);
					printf("PC:\t 0x%04hX\n", reg[R_PC]);
					printf("COND:\t 0x%04hX\n", reg[R_COND]);
				} else if (!strncmp(line, "m", 1)) {
					// todo: implement this
				} else {
					printf("Unrecognized command: %s (type 'help' for help)\n", line);
				}

				// don't leak the line buffer
				linenoiseFree(line);
				disable_input_buffering();
			}
		} 

		switch (op) {
		case OP_ADD:
			{
				// destination register
				uint16_t dr = (instr >> 9) & 0x7;
				// first operand
				uint16_t sr1 = (instr >> 6) & 0x7;
				// whether we are in immediate mode
				uint16_t imm_flag = (instr >> 5) & 0x1;

				if (imm_flag) {
					uint16_t imm5 = sign_extend(instr & 0x1F, 5);
					reg[dr] = reg[sr1] + imm5;
					if (state == S_STEP) printf("ADDed 0x%04hX (SR1) to 0x%04hX (SEXT(imm5)) and stored 0x%04hX (result) in 0x%04hX (DR).\n", sr1, imm5, reg[dr], dr);
				} else {
					uint16_t sr2 = instr & 0x7;
					reg[dr] = reg[sr1] + reg[sr2];
					if (state == S_STEP) printf("ADDed 0x%04hX (SR1) to 0x%04hX (SR2) and stored 0x%04hX (result) in 0x%04hX (DR).\n", sr1, sr2, reg[dr], dr);
				}
				update_flags(dr);
			}

			break;
		case OP_AND:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t sr1 = (instr >> 6) & 0x7;
				uint16_t imm_flag = (instr >> 5) & 0x1;

				if (imm_flag) {
					uint16_t imm5 = sign_extend(instr & 0x1F, 5);
					reg[dr] = reg[sr1] & imm5;
					if (state == S_STEP) printf("ANDed 0x%04hX (SR1) with 0x%04hX (SEXT(imm5)) and stored 0x%04hX (result) in 0x%04hX (DR).\n", sr1, imm5, reg[dr], dr);
				} else {
					uint16_t sr2 = instr & 0x7;
					reg[dr] = reg[sr1] & reg[sr2];
					if (state == S_STEP) printf("ANDed 0x%04hX (SR1) with 0x%04hX (SR2) and stored 0x%04hX (result) in 0x%04hX (DR).\n", sr1, sr2, reg[dr], dr);
				}
				update_flags(dr);
			}

			break;
		case OP_NOT:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t sr = (instr >> 6) & 0x7;

				reg[dr] = ~reg[sr];
				if (state == S_STEP) printf("NOTed 0x%04hX (SR) and stored 0x%04hX (result) in 0x%04hX (DR).\n", sr, reg[dr], dr);
				update_flags(dr);
			}

			break;
		case OP_BR:
			{
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				uint16_t cond_flag = (instr >> 9) & 0x7;
				if (cond_flag & reg[R_COND]) {
					reg[R_PC] += pc_offset;
					if (state == S_STEP) printf("Took BRanch with flag 0x%04hX (n/z/p cond flag) and added 0x%04hX (SEXT(PCoffset9)) to PC.\n", cond_flag, pc_offset);
				} else {
					if (state == S_STEP) printf("Did not take BRanch with flag 0x%04hX (n/z/p cond flag) and offset 0x%04hX (SEXT(PCoffset9)).\n", cond_flag, pc_offset);
				}
			}

			break;
		case OP_JMP:
			{
				// also handles the RET "instruction", which is just when the PC is loaded with the contents of R7
				uint16_t sr = (instr >> 6) & 0x7;
				reg[R_PC] = reg[sr];
				if (state == S_STEP) printf("JMPed (or maybe RETed) to address at contents of 0x%04hX (BaseR).\n", sr);
			}

			break;
		case OP_JSR:
			{
				uint16_t long_flag = (instr >> 11) & 1;
				reg[R_R7] = reg[R_PC];
				if (long_flag) {
					uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11); // JSR
					reg[R_PC] += long_pc_offset;
					if (state == S_STEP) printf("JSRed to PC + 0x%04hX (SEXT(PCoffset11)) and stored incremented PC in R7.\n", long_pc_offset);
				} else {
					uint16_t sr = (instr >> 6) & 0x7;
					reg[R_PC] = reg[sr]; // JSRR
					if (state == S_STEP) printf("JSRRed to address at contents of 0x%04hX (BaseR) and stored incremented PC in R7.\n", sr);
				}
			}

			break;
		case OP_LD:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				reg[dr] = mem_read(reg[R_PC] + pc_offset);
				if (state == S_STEP) printf("LDed contents of address PC + 0x%04hX (SEXT(PCoffset9)) into 0x%04hX (DR).\n", pc_offset, dr);
				update_flags(dr);
			}

			break;
		case OP_LDI:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				// add PC offset to current PC, look at the referenced memory location
				//	to get the final memory location
				reg[dr] = mem_read(mem_read(reg[R_PC] + pc_offset));
				if (state == S_STEP) printf("LDIed contents of address at contents of address PC + 0x%04hX (SEXT(PCoffset9)) into 0x%04hX (DR).\n", pc_offset, dr);
				update_flags(dr);
			}

			break;
		case OP_LDR:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t sr = (instr >> 6) & 0x7;
				uint16_t offset = sign_extend(instr & 0x3F, 6);
				reg[dr] = mem_read(reg[sr] + offset);
				if (state == S_STEP) printf("LDRed contents of address at register 0x%04hX (BaseR) + 0x%04hX (SEXT(offset6)) into 0x%04hX (DR).\n", sr, offset, dr);
				update_flags(dr);
			}

			break;
		case OP_LEA:
			{
				uint16_t dr = (instr >> 9) & 0x7;
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				reg[dr] = reg[R_PC] + pc_offset;
				if (state == S_STEP) printf("LEAed address (not contents of addr.) PC + 0x%04hX (SEXT(PCoffset9)) into 0x%04hX (DR).\n", pc_offset, dr);
				update_flags(dr);
			}

			break;
		case OP_ST: 
			{
				uint16_t sr = (instr >> 9) & 0x7;
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				mem_write(reg[R_PC] + pc_offset, reg[sr]);
				if (state == S_STEP) printf("STed contents of register 0x%04hX (SR) into address PC + 0x%04hX (SEXT(PCoffset9)) = 0x%04hX\n.", sr, pc_offset, reg[R_PC] + pc_offset);
			}

			break;
		case OP_STI:
			{
				uint16_t sr = (instr >> 9) & 0x7;
				uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
				mem_write(mem_read(reg[R_PC] + pc_offset), reg[sr]);
				if (state == S_STEP) printf("STIed contents of register 0x%04hX (SR) into address at contents of address PC + 0x%04hX (SEXT(PCoffset9)).\n", sr, pc_offset);
			}

			break;
		case OP_STR:
			{
				uint16_t sr = (instr >> 9) & 0x7;
				uint16_t baseR = (instr >> 6) & 0x7;
				uint16_t offset = sign_extend(instr & 0x3F, 6);
				mem_write(reg[baseR] + offset, reg[sr]);
				if (state == S_STEP) printf("STRed contents of register 0x%04hX (SR) into address 0x%04hX (SEXT(offset6)) + 0x%04hX (BaseR).\n", sr, offset, baseR);
			}

			break;
		case OP_TRAP:
			{
				reg[R_R7] = reg[R_PC];
				switch (instr & 0xFF) {
				case TRAP_GETC:
					{
						// read a single ASCII char
						reg[R_R0] = (uint16_t) getchar();
						update_flags(R_R0);
					}

					break;
				case TRAP_OUT:
					{
						putc((char) reg[R_R0], stdout);
						fflush(stdout);
					}

					break;
				case TRAP_PUTS:
					{
						// one char per word, not one char per byte
						uint16_t* c = memory + reg[R_R0];
						while (*c) { // TODO: check to make sure we don't overrun the end of the memory array?
							putc((char) *c, stdout);
							++c;
						}
						fflush(stdout);
					}

					break;
				case TRAP_IN:
					{
						printf("Enter a character: ");
						char c = getchar();
						putc(c, stdout);
						fflush(stdout);
						reg[R_R0] = (uint16_t) c;
						update_flags(R_R0);
					}

					break;
				case TRAP_PUTSP:
					{
						// one char per byte here, so two bytes per word.
						//	we need to swap back to big endian
						uint16_t* c = memory + reg[R_R0];
						while (*c) { // TODO: check this
							char char1 = (*c) & 0xFF;
							putc(char1, stdout);
							char char2 = (*c) >> 8;
							if (char2) putc(char2, stdout);
							++c;
						}
						fflush(stdout);
					}

					break;
				case TRAP_HALT:
					{
						puts("HALT");
						fflush(stdout);
						state = S_OFF;
					}

					break;
				default:
					{
						printf("invalid trap vector: 0x%04hX\n", instr & 0xFF);
					}
				}
			}
			if (state == S_STEP) printf("TRAPed with vector 0x%04hX.\n", instr & 0xFF);

			break;
		case OP_RES:
		case OP_RTI: // we disallow the return from interrupt opcode
		default:
			// bad opcode
			printf("illegal opcode: 0x%01hX\n", op);
			goto end;
			break;
		}
	}

end:
	restore_input_buffering();
}
