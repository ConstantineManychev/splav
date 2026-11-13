#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "tests/test.hpp"

#include <optional>
#include <string>

SPLAV_TEST(readsCorpusByCyrillicPath, "поддержка: файл корпуса читается по кириллическому пути")
{
    const std::optional<std::string> source =
        splav::readFile(context.root() / splav::pathFromUtf8("корпус/json/вход.спл"));
    context.expect(source.has_value(), "корпус/json/вход.спл открывается");
    if (source)
    {
        context.expect(source->starts_with("подключить"), "файл начинается со слова «подключить»");
    }
}

SPLAV_TEST(keepsUtf8PathsIntact, "поддержка: путь в UTF-8 переживает преобразование туда и обратно")
{
    const std::string original = "корпус/злые/01-сломатьПамять.спл";
    context.expectEqual(splav::utf8FromPath(splav::pathFromUtf8(original)), original, "путь");
}

SPLAV_TEST(reportsMissingFile, "поддержка: отсутствующий файл не читается")
{
    const std::optional<std::string> source =
        splav::readFile(context.root() / splav::pathFromUtf8("корпус/нет-такого.спл"));
    context.expect(!source.has_value(), "несуществующий файл даёт пустой результат");
}
