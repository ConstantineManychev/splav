#include "interp/interpreter.hpp"
#include "sema/program.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "tests/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace splav;

struct Execution
{
    int code = 0;
    std::string output;
    std::string errors;
};

RunOptions capturing(Execution& execution, std::vector<std::string> arguments)
{
    RunOptions options;
    options.arguments = std::move(arguments);
    options.write = [&execution](bool error, std::string_view text)
    { (error ? execution.errors : execution.output) += text; };
    return options;
}

Execution runSource(test::Context& context, const std::string& source)
{
    Program program(context.root(), context.root() / pathFromUtf8("библиотека"));
    Module* module = program.loadSource(source, "проба");
    Execution execution;
    if (module == nullptr || !module->syntax.empty())
    {
        context.fail("проба не разбирается");
        return execution;
    }
    execution.code = runProgram(program, *module, capturing(execution, {"проба"}));
    return execution;
}

Execution runPackage(test::Context& context, const std::filesystem::path& root, std::string_view package,
    std::vector<std::string> arguments)
{
    const std::filesystem::path entry = root / pathFromUtf8(package) / pathFromUtf8("вход.спл");
    Program program(findPackageRoot(entry), root / pathFromUtf8("библиотека"));
    Module* module = program.loadEntry(entry);
    Execution execution;
    if (module == nullptr)
    {
        context.fail("пакет не загружается");
        return execution;
    }
    execution.code = runProgram(program, *module, capturing(execution, std::move(arguments)));
    return execution;
}

void expectTests(test::Context& context, std::string_view file, size_t expected)
{
    const std::filesystem::path path = context.root() / pathFromUtf8(file);
    Program program(findPackageRoot(path), context.root() / pathFromUtf8("библиотека"));
    Module* module = program.loadEntry(path);
    if (module == nullptr)
    {
        context.fail("модуль не загружается");
        return;
    }
    Execution execution;
    const std::vector<TestOutcome> outcomes = runTests(program, *module, capturing(execution, {}));
    context.expectEqual(outcomes.size(), expected, std::string(file) + ": число тестов");
    for (const TestOutcome& outcome : outcomes)
    {
        context.expect(outcome.passed, outcome.name + ": " + outcome.message);
    }
}

SPLAV_TEST(printsAndExits, "интерпретатор: печать и код выхода")
{
    const Execution execution = runSource(context, R"(подключить ядро.консоль
подключить ядро.ос

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    изм сумма: ц64 = 0
    цикл число в 1..=10
    {
        сумма += число
    }
    попытка консоль.печать("сумма ", сумма)
    возврат Успех()
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.output, std::string("сумма 55\n"), "вывод");
}

SPLAV_TEST(overflowTraps, "интерпретатор: переполнение — ловушка с местом")
{
    const Execution execution = runSource(context, R"(функ вход()
{
    изм байт: н8 = 250
    цикл _ в 0..10
    {
        байт += 1
    }
}
)");
    context.expectEqual(execution.code, PANIC_EXIT_CODE, "код выхода");
    context.expectEqual(
        execution.errors, std::string("паника: переполнение `н8`: 255 + 1\n  в проба:6:14\n"), "сообщение паники");
}

SPLAV_TEST(wrappingAndChecked, "интерпретатор: заворачивание и проверяемая арифметика")
{
    const Execution execution = runSource(context, R"(подключить ядро.консоль
подключить ядро.ос

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    байт: н8 = 250
    завёрнуто = байт +% 10
    проверено: ?н8 = байт +? 10
    попытка консоль.печать(завёрнуто, " ", проверено это Ничего, " ", (байт как ц32) - 300)
    возврат Успех()
}
)");
    context.expectEqual(execution.output, std::string("4 да -50\n"), "вывод");
}

SPLAV_TEST(controlFlowAndPatterns, "интерпретатор: метки, `отложить`, образцы и `?Т`")
{
    const Execution execution = runSource(context, R"(подключить ядро.консоль
подключить ядро.ос
подключить ядро.текст

структ Точка
{
    х: ц32
    у: ц32
}

переч Фигура
{
    Круг(радиус: ц32)
    Отрезок(Точка, Точка)
    Пусто
}

функ Фигура.вид(&сам) требует Регион -> стр
{
    возврат выбор сам
    {
        Круг(р) если р > 10       => "большой круг"
        Круг(_)                   => "круг"
        Отрезок(а, Точка{ х, у }) => текст.формат("отрезок до {},{} от {}", х, у, а.х)
        Пусто                     => "пусто"
    }
}

функ половина(число: ц32) -> ?ц32
{
    если число % 2 != 0
    {
        возврат Ничего
    }
    возврат число / 2
}

функ четверть(число: ц32) -> ?ц32
{
    половинка = попытка половина(число)
    возврат половина(половинка)
}

функ сдвинуть(точка: &изм Точка)
{
    точка.х += 1
}

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    изм найдено: ц32 = 0
    внешний: цикл а в 0..10
    {
        цикл б в 0..10
        {
            если б > а
            {
                продолжить внешний
            }
            если а * б == 12
            {
                найдено = а * 10 + б
                прервать внешний
            }
        }
    }
    изм точка = Точка{ х: 1, у: 2 }
    сдвинуть(&изм точка)
    (левое, правое) = (точка.х, точка.у)
    регион
    {
        попытка консоль.печать(найдено, " ", левое, правое)
        попытка консоль.печать(Круг(3).вид(), "; ", Круг(30).вид(), "; ", Отрезок(Точка{ х: 0, у: 0 }, точка).вид())
        попытка консоль.печать(четверть(12) это Есть(3), " ", четверть(6) это Ничего)
        строка = "е\u{301}ж👨‍👩‍👧!"
        попытка консоль.печать(строка.графемы().количество(), " ", строка.срез(0..4))
        цикл символ в 'а'..='в'
        {
            отложить консоль.печать("после ", символ)
            попытка консоль.печать("до ", символ)
        }
    }
    возврат Успех()
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string(), "ошибки");
    context.expectEqual(execution.output,
        std::string("43 22\nкруг; большой круг; отрезок до 2,2 от 0\nда да\n4 е́\n"
                    "до а\nпосле а\nдо б\nпосле б\nдо в\nпосле в\n"),
        "вывод");
}

SPLAV_TEST(errorEntryExitsWithOne, "интерпретатор: `Ошибка` из `вход` — код 1 без вывода")
{
    const Execution execution = runSource(context, R"(переч Сбой
{
    Всегда
}

функ вход() -> Итог(пусто, Сбой)
{
    возврат Ошибка(Всегда)
}
)");
    context.expectEqual(execution.code, FAILURE_EXIT_CODE, "код выхода");
    context.expectEqual(execution.output + execution.errors, std::string(), "вывод");
}

SPLAV_TEST(jsonCorpusRuns, "интерпретатор: корпус json проверяет документ")
{
    const std::filesystem::path document =
        std::filesystem::temp_directory_path() / pathFromUtf8("сплав-проба-json.json");
    writeFile(document, R"({"имя": "Сплав", "версии": [1, 2.5, -0.5], "вложено": {"а": null, "б": true}})");
    const Execution valid =
        runPackage(context, context.root(), "корпус/json", {"проверить-json", utf8FromPath(document)});
    context.expectEqual(valid.code, 0, "код выхода для корректного документа");
    context.expectEqual(valid.output, std::string("корректный JSON, значений: 9\n"), "вывод");
    writeFile(document, "{\n  \"ключ\": [1, 2,,]\n}");
    const Execution broken =
        runPackage(context, context.root(), "корпус/json", {"проверить-json", utf8FromPath(document)});
    context.expectEqual(broken.code, FAILURE_EXIT_CODE, "код выхода для испорченного документа");
    context.expectEqual(broken.errors, std::string("2:17: неожиданный символ\n"), "сообщение");
    std::filesystem::remove(document);
}

SPLAV_TEST(jsonCorpusTests, "интерпретатор: тесты корпуса json")
{
    expectTests(context, "корпус/json/json.спл", 7);
}

SPLAV_TEST(ecsCorpusRuns, "интерпретатор: корпус ecs проигрывает 600 кадров")
{
    const std::filesystem::path previous = std::filesystem::current_path();
    const std::filesystem::path root = std::filesystem::absolute(context.root());
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / pathFromUtf8("сплав-проба-ecs");
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    const Execution execution = runPackage(context, root, "корпус/ecs", {"ecs"});
    const std::optional<std::string> frames = readFile(directory / pathFromUtf8("кадры.txt"));
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(directory);
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string("счёт: 16\nпоследняя запись: кадр 540, записей 10\n"), "журнал");
    context.expect(frames.has_value() && frames->starts_with("кадр 0: врагов 64, пуль 1, здоровье 192, счёт 0\n"),
        "первая строка кадров");
    context.expect(frames.has_value() && frames->ends_with("кадр 540: врагов 49, пуль 8, здоровье 145, счёт 15\n"),
        "последняя строка кадров");
}

SPLAV_TEST(ecsCorpusTests, "интерпретатор: тесты корпуса ecs")
{
    expectTests(context, "корпус/ecs/ecs.спл", 3);
}

}
