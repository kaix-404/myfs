#include<iostream>
#include<fstream>
#include<string>

int main() {
    // Write using C++ streams
    std::ofstream outfile("mnt/from_cpp.txt");
    if (outfile.is_open()) {
        outfile << "Hello from C++ standard library!" << std::endl;
        outfile.close();
        std::cout << "[C++] File written successfully." << std::endl;
    }

    // Read back
    std::ifstream infile("mnt/from_cpp.txt");
    std::string line;
    if (infile.is_open()) {
        while (std::getline(infile, line)) {
            std::cout << "[C++] Read line: " << line << std::endl;
        }
        infile.close();
    }
    return 0;
}
