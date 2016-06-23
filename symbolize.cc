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
