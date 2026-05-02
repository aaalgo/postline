// test_check_stack.cpp
#include <execinfo.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <postline/common.h>

using namespace postline;


void level3(int x)
{
    CHECK(x == 42);
}

void level2(int x)
{
    level3(x);
}

void level1()
{
    level2(7);
}

int main()
{
    level1();
    return 0;
}
