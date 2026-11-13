#include "driver/driver.hpp"
#include "support/platform.hpp"

int main(int argc, char** argv)
{
    splav::prepareStandardStreams();
    return splav::runDriver(splav::utf8Arguments(argc, argv));
}
