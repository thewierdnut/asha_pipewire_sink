#pragma once

#include <iostream>
#include <sstream>

namespace unit_test
{
    class TestFailure: public std::stringstream
    {
    public:
        TestFailure(const char* filename, unsigned int linenumber, const char* statement):
            m_filename(filename), m_linenumber(linenumber), m_statement(statement) {}
        ~TestFailure()
        {
            std::cout << m_filename << ":" << m_linenumber << ": error: \"" << m_statement << "\" was false.\n";
            if (!str().empty())
                std::cout << "    " << str() << "\n";
            exit(1);
        }

    private:
        const char* m_filename;
        unsigned int m_linenumber;
        const char* m_statement;
    };
}

#define ASSERT_TRUE(...) if (!!(__VA_ARGS__)) {} else unit_test::TestFailure(__FILE__, __LINE__, #__VA_ARGS__)