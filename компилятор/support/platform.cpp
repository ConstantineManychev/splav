#include "support/platform.hpp"

#ifdef _WIN32
#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <shellapi.h>
#else
#include <pthread.h>
#endif

namespace splav
{

namespace
{

struct StackWork
{
    const std::function<void()>* work;
};

}

#ifdef _WIN32

namespace
{

std::string utf8FromWide(const wchar_t* text)
{
    const int sizeWithTerminator = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (sizeWithTerminator <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(sizeWithTerminator - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), sizeWithTerminator - 1, nullptr, nullptr);
    return result;
}

}

std::vector<std::string> utf8Arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    if (wide == nullptr)
    {
        return arguments;
    }
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        arguments.push_back(utf8FromWide(wide[index]));
    }
    LocalFree(wide);
    return arguments;
}

void prepareStandardStreams()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

namespace
{

unsigned __stdcall runStackWork(void* argument)
{
    (*static_cast<StackWork*>(argument)->work)();
    return 0;
}

}

void runWithStack(size_t bytes, const std::function<void()>& work)
{
    StackWork context{&work};
    const uintptr_t handle = _beginthreadex(
        nullptr, static_cast<unsigned>(bytes), runStackWork, &context, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0)
    {
        work();
        return;
    }
    WaitForSingleObject(reinterpret_cast<HANDLE>(handle), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

#else

namespace
{

void* runStackWork(void* argument)
{
    (*static_cast<StackWork*>(argument)->work)();
    return nullptr;
}

}

void runWithStack(size_t bytes, const std::function<void()>& work)
{
    StackWork context{&work};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, bytes);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, runStackWork, &context);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        work();
        return;
    }
    pthread_join(thread, nullptr);
}

std::vector<std::string> utf8Arguments(int argc, char** argv)
{
    return std::vector<std::string>(argv, argv + argc);
}

void prepareStandardStreams()
{
}

#endif

}
