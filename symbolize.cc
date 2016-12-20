/*
 * symbol resolver (for use with uniprof)
 *
 * Authors: Florian Schmidt <florian.schmidt@neclab.eu>
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */

#include <stdio.h>
#include <inttypes.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

int main(int argc, char **argv) {
	std::ifstream symbolfile;
	std::ifstream tracefile;
	std::string line;
	std::stringstream convertor;
	uint64_t addr;
	std::string hexaddr;
	char type;
	std::string fname;
	std::map<uint64_t, std::string> symbollist;
	std::map<uint64_t, std::string>::iterator iter;

	if (argc != 3) {
		std::cout << "Usage: " << argv[0] << " <symbol_table> <trace_file>" << std::endl;
		return 1;
	}

	symbolfile.open(argv[1], std::ifstream::in);
	if (symbolfile.fail()) {
		std::cout << "Failed opening symbol table file \"" << argv[1] << "\".";
		return 2;
	}
	tracefile.open(argv[2], std::ifstream::in);
	if (tracefile.fail()) {
		std::cout << "Failed opening trace file \"" << argv[2] << "\".";
		return 2;
	}

	while (std::getline(symbolfile, line)) {
		convertor.str(line);
		convertor >> std::hex >> addr >> type >> fname;
		convertor.clear();
		symbollist[addr] = fname;
	}
	symbolfile.close();

	while (std::getline(tracefile, line)) {
		if (line.empty())
			std::cout << std::endl;
		else if (line == "1")
			std::cout << "1" << std::endl;
		// comments; by convention, the header lines start with a comment sign
		else if (line[0] == '#')
			std::cout << line << std::endl;
		else {
			convertor.str(line);
			convertor >> std::hex >> addr;
			convertor.clear();
			iter = symbollist.upper_bound(addr);
			if (iter != symbollist.begin())
				iter--;
			if (addr == iter->first)
				std::cout << iter->second.c_str() << std::endl;
			else
				std::cout << iter->second.c_str() << "+0x" << std::hex << addr - iter->first << std::endl;
		}
	}
	tracefile.close();

	return 0;
}
