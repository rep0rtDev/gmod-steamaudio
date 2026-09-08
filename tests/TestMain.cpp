// tests/TestMain.cpp
#include "TestFramework.h"

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return satest::RunAll(argc, argv);
}
