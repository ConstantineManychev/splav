#include "interp/interpreter.hpp"
#include "sema/checker.hpp"
#include "sema/names.hpp"
#include "sema/program.hpp"
#include "sema/report.hpp"
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

#ifdef _WIN32
constexpr const char* SEPARATOR = "\\";
#else
constexpr const char* SEPARATOR = "/";
#endif

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

Execution runChecked(test::Context& context, const std::string& source)
{
    Program program(context.root(), context.root() / pathFromUtf8("библиотека"));
    Module* module = program.loadSource(source, "проба");
    Execution execution;
    if (module == nullptr || !module->syntax.empty())
    {
        context.fail("проба не разбирается");
        return execution;
    }
    std::vector<Report> reports = resolveNames(program, *module, Language::Russian);
    if (reports.empty())
    {
        reports = checkTypes(program, *module, Language::Russian);
    }
    for (const Report& report : reports)
    {
        context.fail("проверка: " + renderReport(report, module->source, Language::Russian));
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

SPLAV_TEST(specClosures, "интерпретатор: замыкания из §12.2")
{
    const Execution execution = runChecked(context, R"(подключить ядро.консоль
подключить ядро.ос

функ сумма(значения: []н32, отбор: &функ(н32) -> бул) -> н64
{
    изм накоплено: н64 = 0
    цикл значение в значения
    {
        если отбор(значение)
        {
            накоплено += значение как н64
        }
    }
    возврат накоплено
}

функ суммаБольших(значения: []н32, порог: н32) -> н64
{
    возврат сумма(значения, функ(х: н32) => х > порог)
}

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    числа = [_]н32{ 1, 5, 10, 20 }
    изм счётчик: ц32 = 0
    увеличить = функ()
    {
        счётчик += 1
    }
    увеличить()
    увеличить()
    снимок = функ[копия]() => счётчик
    счётчик += 10
    попытка консоль.печать(суммаБольших(числа[..], 4), " ", счётчик, " ", снимок())
    возврат Успех()
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string(), "ошибки");
    context.expectEqual(execution.output, std::string("35 12 2\n"), "вывод");
}

SPLAV_TEST(specComptime, "интерпретатор: обобщения и `компил` из §12.5")
{
    const Execution execution = runChecked(context, R"(подключить ядро.консоль
подключить ядро.ос

функ Пара(компил Л: тип, компил П: тип) -> тип
{
    возврат структ
    {
        левый: Л
        правый: П
    }
}

функ поменять(компил Л: тип, компил П: тип, пара: Пара(Л, П)) -> Пара(П, Л)
{
    возврат Пара(П, Л){ левый: пара.правый, правый: пара.левый }
}

функ таблицаКвадратов(размер: разм) требует Регион -> Массив(н32)
{
    изм таблица = Массив(н32).новый()
    цикл число в 0..размер
    {
        таблица.добавить((число * число) как н32)
    }
    возврат таблица
}

конст КВАДРАТЫ = таблицаКвадратов(16)

функ наибольшее(компил Т: тип, а: Т, б: Т) -> Т
{
    если а > б
    {
        возврат а
    }
    возврат б
}

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    пара = Пара(ц32, стр){ левый: 7, правый: "семь" }
    обратная = поменять(ц32, стр, пара)
    попытка консоль.печать(обратная.левый, " ", обратная.правый, " ", Пара(ц32, стр) == Пара(ц32, стр), " ",
        Пара(ц32, стр) == Пара(стр, ц32))
    попытка консоль.печать(наибольшее(3, 9), " ", наибольшее(ц64, -1, -5), " ", компил КВАДРАТЫ[12])
    попытка напечататьРазделитель()
    возврат Успех()
}

функ разделительПути() -> сим
{
    компил если компил_цель().ос == Ос.Windows
    {
        возврат '\\'
    }
    возврат '/'
}

функ напечататьРазделитель() -> Итог(пусто, ос.ОшибкаВвода)
{
    возврат консоль.печать(разделительПути())
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string(), "ошибки");
    context.expectEqual(execution.output, std::string("семь 7 да нет\n9 -1 144\n") + SEPARATOR + "\n", "вывод");
}

SPLAV_TEST(specTraits, "интерпретатор: типажи, `дин` и `Уник` из §12.3")
{
    const Execution execution = runChecked(context, R"(подключить ядро.консоль
подключить ядро.ос

откр типаж Чтение
{
    функ читать(&изм сам, буфер: []изм н8) -> Итог(разм, ос.ОшибкаВвода)
}

структ Источник
{
    данные: стр
    позиция: разм
}

функ Источник.читать(&изм сам, буфер: []изм н8) -> Итог(разм, ос.ОшибкаВвода)
{
    байты = сам.данные.байты()
    изм прочитано: разм = 0
    цикл прочитано < буфер.длина и сам.позиция < байты.длина
    {
        буфер[прочитано] = байты[сам.позиция]
        прочитано += 1
        сам.позиция += 1
    }
    возврат Успех(прочитано)
}

реализ Чтение для Источник

функ прочитатьВсё(источник: &изм дин Чтение, буфер: []изм н8) -> Итог(разм, ос.ОшибкаВвода)
{
    изм всего: разм = 0
    цикл
    {
        прочитано = попытка источник.читать(буфер[всего..])
        если прочитано == 0
        {
            возврат Успех(всего)
        }
        всего += прочитано
    }
}

структ Точка
{
    х: ц32
    у: ц32
}

реализ Равенство для Точка

реализ Сравнение для Точка

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    изм источник = Источник{ данные: "привет", позиция: 0 }
    изм буфер: [64]н8
    всего = попытка прочитатьВсё(&изм источник, буфер[..])
    изм коробка = Уник.новый(Источник{ данные: "ab", позиция: 0 })
    изм малый: [1]н8
    первый = попытка коробка.читать(малый[..])
    коробка.освободить()
    попытка консоль.печать(всего, " ", первый, " ", малый[0])
    попытка консоль.печать(Точка{ х: 1, у: 2 } < Точка{ х: 1, у: 3 }, " ", Точка{ х: 2, у: 0 } == Точка{ х: 2, у: 0 })
    возврат Успех()
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string(), "ошибки");
    context.expectEqual(execution.output, std::string("12 1 97\nда да\n"), "вывод");
}

SPLAV_TEST(specOperators, "интерпретатор: операторы через типажи и векторы из §12.6")
{
    const Execution execution = runChecked(context, R"(подключить ядро.консоль
подключить ядро.ос

структ Комплекс
{
    вещ: в64
    мним: в64
}

функ Комплекс.сложить(сам, другое: Комплекс) -> Комплекс
{
    возврат Комплекс{ вещ: сам.вещ + другое.вещ, мним: сам.мним + другое.мним }
}

реализ Сложение для Комплекс

конст Век3 = Вектор(в32, 3)

структ Матрица3
{
    строки: [3]Век3
}

функ скалярное(а: Век3, б: Век3) -> в32
{
    возврат а.х * б.х + а.у * б.у + а.з * б.з
}

реализ Умножение(Век3, Век3) для Матрица3
{
    функ умножить(сам, вектор: Век3) -> Век3
    {
        возврат Век3{
            скалярное(сам.строки[0], вектор),
            скалярное(сам.строки[1], вектор),
            скалярное(сам.строки[2], вектор),
        }
    }
}

функ вход() -> Итог(пусто, ос.ОшибкаВвода)
{
    с = Комплекс{ вещ: 1.0, мним: 2.0 } + Комплекс{ вещ: 0.5, мним: -1.0 }
    м = Матрица3{ строки: [3]Век3{ Век3{ 1.0, 0.0, 0.0 }, Век3{ 0.0, 2.0, 0.0 }, Век3{ 0.0, 0.0, 3.0 } } }
    р = м * Век3{ 1.0, 1.0, 1.0 }
    попытка консоль.печать(с.вещ, " ", с.мним, " ", р.х, " ", р.у, " ", р.з)
    возврат Успех()
}
)");
    context.expectEqual(execution.code, 0, "код выхода");
    context.expectEqual(execution.errors, std::string(), "ошибки");
    context.expectEqual(execution.output, std::string("1.5 1 1 2 3\n"), "вывод");
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
