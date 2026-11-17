#include "interp/machine.hpp"

#include "sema/naming.hpp"
#include "support/file_io.hpp"
#include "support/utf8_path.hpp"
#include "unicode/unicode.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace splav
{

namespace
{

constexpr char32_t LAST_SCALAR = 0x10FFFF;
constexpr char32_t FIRST_SURROGATE = 0xD800;
constexpr char32_t LAST_SURROGATE = 0xDFFF;

bool isScalar(uint64_t code)
{
    return code <= LAST_SCALAR && (code < FIRST_SURROGATE || code > LAST_SURROGATE);
}

Cell sizeCell(size_t value)
{
    return integerCell(unsignedInteger(value), Number::Size);
}

Cell makeView(ValueKind kind, const Value& source, size_t start, size_t length)
{
    return viewCell(kind, source.text, source.start + start, length);
}

std::string& builderText(Call& call, const Cell& cell)
{
    const Cell& target = dereference(cell);
    if (target->kind != ValueKind::Builder)
    {
        call.machine.panic("ожидался `текст.Построитель`");
    }
    return *target->builder;
}

std::string concatenated(Call& call, size_t from)
{
    std::string text;
    for (size_t index = from; index < call.arguments.size(); ++index)
    {
        text += call.machine.show(call.arguments[index]);
    }
    return text;
}

Cell inputError(Call& call, std::string_view variant)
{
    return call.machine.failure(call.machine.enumValue(call.machine.inputErrorType, variant, {}));
}

Cell testFailure(Call& call, const std::string& message)
{
    return call.machine.failure(call.machine.structValue(call.machine.failureType, {stringCell(message)}));
}

bool isResult(Call& call, const Value& value, std::string_view variant)
{
    return value.kind == ValueKind::Enum && value.decl == call.machine.resultType
        && value.decl->variants[value.variant].key == variant;
}

double floatOf(Call& call, const Value& value)
{
    if (value.kind == ValueKind::Float)
    {
        return value.floating;
    }
    if (value.kind == ValueKind::Integer)
    {
        return integerToDouble(value.integer);
    }
    call.machine.panic("ожидалось число");
}

Number floatNumberOf(const Value& value)
{
    return value.number == Number::F32 ? Number::F32 : Number::F64;
}

size_t sizeArgument(Call& call, size_t index)
{
    const Value& value = call.argument(index);
    if (value.kind != ValueKind::Integer || value.integer.negative)
    {
        call.machine.panic("ожидалось неотрицательное целое");
    }
    return static_cast<size_t>(value.integer.magnitude);
}

Cell arrayNew(Call&)
{
    return listCell({}, true);
}

Cell arrayAdd(Call& call)
{
    if (call.arguments.empty())
    {
        call.machine.panic("`добавить` без элемента");
    }
    call.receiver->storage->push_back(copyCell(call.arguments.front()));
    return voidCell();
}

Cell arrayCount(Call& call)
{
    return sizeCell(listLength(*call.receiver));
}

Cell arrayPop(Call& call)
{
    std::vector<Cell>& storage = *call.receiver->storage;
    if (storage.empty())
    {
        return optionalCell(nullptr);
    }
    Cell last = storage.back();
    storage.pop_back();
    return optionalCell(std::move(last));
}

Cell arraySwapRemove(Call& call)
{
    std::vector<Cell>& storage = *call.receiver->storage;
    const size_t position = sizeArgument(call, 0);
    if (position >= storage.size())
    {
        call.machine.panic(
            "индекс " + std::to_string(position) + " вне массива длины " + std::to_string(storage.size()));
    }
    Cell removed = storage[position];
    storage[position] = storage.back();
    storage.pop_back();
    return removed;
}

Cell arrayIterate(Call& call)
{
    Cell view = copyCell(call.receiver);
    view->growable = false;
    view->start = 0;
    view->length = view->storage->size();
    return view;
}

Cell arrayEnumerate(Call& call)
{
    Value value;
    value.kind = ValueKind::Enumeration;
    value.target = call.receiver;
    return makeCell(std::move(value));
}

Cell graphemesCount(Call& call)
{
    const std::string_view text = textView(*call.receiver);
    size_t count = 0;
    for (size_t position = 0; position < text.size(); position = unicode::graphemeEnd(text, position))
    {
        ++count;
    }
    return sizeCell(count);
}

Cell graphemesNext(Call& call)
{
    Value& source = *call.receiver;
    const std::string_view text = textView(source);
    if (text.empty())
    {
        return optionalCell(nullptr);
    }
    const size_t end = unicode::graphemeEnd(text, 0);
    Cell piece = makeView(ValueKind::Str, source, 0, end);
    source.start += end;
    source.length -= end;
    return optionalCell(std::move(piece));
}

Cell graphemesTake(Call& call)
{
    const Value& source = *call.receiver;
    const std::string_view text = textView(source);
    const size_t wanted = sizeArgument(call, 0);
    size_t position = 0;
    for (size_t taken = 0; taken < wanted && position < text.size(); ++taken)
    {
        position = unicode::graphemeEnd(text, position);
    }
    return makeView(ValueKind::Graphemes, source, 0, position);
}

Cell charsNext(Call& call)
{
    Value& source = *call.receiver;
    const std::string_view text = textView(source);
    if (text.empty())
    {
        return optionalCell(nullptr);
    }
    const unicode::Decoded decoded = unicode::decodeUtf8(text, 0);
    source.start += decoded.length;
    source.length -= decoded.length;
    return optionalCell(charCell(decoded.codePoint));
}

Cell stringBytes(Call& call)
{
    return makeView(ValueKind::Bytes, *call.receiver, 0, call.receiver->length);
}

Cell stringSlice(Call& call)
{
    const Value& source = *call.receiver;
    const Value& bounds = call.argument(0);
    if (bounds.kind != ValueKind::Range)
    {
        call.machine.panic("`срез` ждёт диапазон");
    }
    const auto boundOf = [&](const Cell& cell, size_t fallback)
    {
        if (!cell)
        {
            return fallback;
        }
        const Value& value = *dereference(cell);
        if (value.kind != ValueKind::Integer || value.integer.negative)
        {
            call.machine.panic("граница среза — не размер");
        }
        return static_cast<size_t>(value.integer.magnitude);
    };
    const size_t from = boundOf(bounds.items[0], 0);
    const size_t to = boundOf(bounds.items[1], source.length) + (bounds.items[1] && bounds.inclusive ? 1 : 0);
    if (from > to || to > source.length)
    {
        call.machine.panic("границы " + std::to_string(from) + ".." + std::to_string(to) + " вне строки длины "
            + std::to_string(source.length));
    }
    const std::string_view text = textView(source);
    for (const size_t edge : {from, to})
    {
        if (edge < text.size() && (static_cast<unsigned char>(text[edge]) & 0xC0) == 0x80)
        {
            call.machine.panic("граница " + std::to_string(edge) + " делит символ UTF-8");
        }
    }
    return makeView(ValueKind::Str, source, from, to - from);
}

Cell stringGraphemes(Call& call)
{
    return makeView(ValueKind::Graphemes, *call.receiver, 0, call.receiver->length);
}

Cell stringChars(Call& call)
{
    return makeView(ValueKind::Chars, *call.receiver, 0, call.receiver->length);
}

std::string bytesOf(Call& call, const Value& value)
{
    if (value.kind == ValueKind::Bytes || value.kind == ValueKind::Str)
    {
        return std::string(textView(value));
    }
    if (value.kind == ValueKind::List)
    {
        std::string bytes;
        for (size_t index = 0; index < listLength(value); ++index)
        {
            const Value& item = *dereference(listElement(value, index));
            bytes += static_cast<char>(item.integer.magnitude & 0xFF);
        }
        return bytes;
    }
    call.machine.panic("ожидался срез байтов");
}

Cell stringFromUtf8(Call& call)
{
    std::string bytes = bytesOf(call, call.argument(0));
    const TypeDecl* errorType = call.machine.findType("ядро.прелюдия", "ОшибкаUtf8");
    for (size_t position = 0; position < bytes.size();)
    {
        const unicode::Decoded decoded = unicode::decodeUtf8(bytes, position);
        if (!decoded.valid)
        {
            const unsigned char lead = static_cast<unsigned char>(bytes[position]);
            const size_t expected = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (expected > 1 && position + expected > bytes.size())
            {
                return call.machine.failure(call.machine.enumValue(errorType, "ОборванаПоследовательность", {}));
            }
            return call.machine.failure(
                call.machine.enumValue(errorType, "НеверныйБайт", {sizeCell(position)}));
        }
        position += decoded.length;
    }
    return call.machine.success(stringCell(std::move(bytes)));
}

Cell charFromCode(Call& call)
{
    const Value& code = call.argument(0);
    if (code.kind != ValueKind::Integer || code.integer.negative || !isScalar(code.integer.magnitude))
    {
        return optionalCell(nullptr);
    }
    return optionalCell(charCell(static_cast<char32_t>(code.integer.magnitude)));
}

Cell float32Bits(Call& call)
{
    const float value = static_cast<float>(call.receiver->floating);
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return integerCell(unsignedInteger(bits), Number::U32);
}

Cell float64Bits(Call& call)
{
    const double value = call.receiver->floating;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return integerCell(unsignedInteger(bits), Number::U64);
}

Cell floatFinite(Call& call)
{
    return boolCell(std::isfinite(call.receiver->floating));
}

Cell systemArguments(Call& call)
{
    std::vector<Cell> items;
    for (const std::string& argument : call.machine.options.arguments)
    {
        items.push_back(stringCell(argument));
    }
    return listCell(std::move(items), false);
}

Cell readText(Call& call)
{
    const std::filesystem::path path = pathFromUtf8(textView(call.argument(0)));
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return inputError(call, "НеНайдено");
    }
    std::optional<std::string> contents = readFile(path);
    if (!contents)
    {
        return inputError(call, "НетДоступа");
    }
    for (size_t position = 0; position < contents->size();)
    {
        const unicode::Decoded decoded = unicode::decodeUtf8(*contents, position);
        if (!decoded.valid)
        {
            return inputError(call, "НеверныеДанные");
        }
        position += decoded.length;
    }
    return call.machine.success(stringCell(std::move(*contents)));
}

Cell consolePrint(Call& call)
{
    call.machine.write(false, concatenated(call, 0) + "\n");
    return call.machine.success(nullptr);
}

Cell consoleError(Call& call)
{
    call.machine.write(true, concatenated(call, 0) + "\n");
    return call.machine.success(nullptr);
}

Cell logInform(Call& call)
{
    call.machine.write(true, concatenated(call, 0) + "\n");
    return voidCell();
}

Cell logWarn(Call& call)
{
    call.machine.write(true, "предупреждение: " + concatenated(call, 0) + "\n");
    return voidCell();
}

Cell logError(Call& call)
{
    call.machine.write(true, "ошибка: " + concatenated(call, 0) + "\n");
    return voidCell();
}

Cell builderNew(Call&)
{
    Value value;
    value.kind = ValueKind::Builder;
    value.builder = std::make_shared<std::string>();
    return makeCell(std::move(value));
}

Cell builderAdd(Call& call)
{
    *call.receiver->builder += textView(call.argument(0));
    return voidCell();
}

Cell builderAddChar(Call& call)
{
    unicode::appendUtf8(*call.receiver->builder, call.argument(0).character);
    return voidCell();
}

Cell builderDone(Call& call)
{
    return stringCell(*call.receiver->builder);
}

Cell textFormat(Call& call)
{
    const std::string_view pattern = textView(call.argument(0));
    std::string text;
    size_t next = 1;
    for (size_t position = 0; position < pattern.size(); ++position)
    {
        const char symbol = pattern[position];
        const char following = position + 1 < pattern.size() ? pattern[position + 1] : '\0';
        if ((symbol == '{' && following == '{') || (symbol == '}' && following == '}'))
        {
            text += symbol;
            ++position;
        }
        else if (symbol == '{' && following == '}')
        {
            if (next >= call.arguments.size())
            {
                call.machine.panic("в `формат` меньше частей, чем `{}` в шаблоне");
            }
            text += call.machine.show(call.arguments[next++]);
            ++position;
        }
        else
        {
            text += symbol;
        }
    }
    if (next != call.arguments.size())
    {
        call.machine.panic("в `формат` больше частей, чем `{}` в шаблоне");
    }
    return stringCell(std::move(text));
}

bool isDecimalNumber(std::string_view text)
{
    size_t position = text.starts_with('-') ? 1 : 0;
    const auto digits = [&]
    {
        const size_t start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        {
            ++position;
        }
        return position > start;
    };
    if (!digits())
    {
        return false;
    }
    if (position < text.size() && text[position] == '.')
    {
        ++position;
        if (!digits())
        {
            return false;
        }
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E'))
    {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-'))
        {
            ++position;
        }
        if (!digits())
        {
            return false;
        }
    }
    return position == text.size();
}

Cell parseFloat(Call& call)
{
    const std::string text(textView(call.argument(0)));
    if (!isDecimalNumber(text))
    {
        return optionalCell(nullptr);
    }
    double value = 0;
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc::result_out_of_range)
    {
        value = std::strtod(text.c_str(), nullptr);
    }
    return optionalCell(floatCell(value, Number::F64));
}

Cell writeInteger(Call& call)
{
    builderText(call, call.arguments.at(0)) += integerText(call.argument(1).integer);
    return voidCell();
}

Cell writeFloat(Call& call)
{
    builderText(call, call.arguments.at(0)) += floatText(call.argument(1).floating, Number::F64);
    return voidCell();
}

Cell expectTrue(Call& call)
{
    const Value& condition = call.argument(0);
    if (condition.kind == ValueKind::Bool && condition.truth)
    {
        return call.machine.success(nullptr);
    }
    return testFailure(call, "условие не выполнено");
}

Cell expectEqual(Call& call)
{
    const Cell& left = call.argumentCell(0);
    const Cell& right = call.argumentCell(1);
    if (call.machine.equal(left, right))
    {
        return call.machine.success(nullptr);
    }
    return testFailure(
        call, "ожидалось равенство: " + call.machine.describe(left) + " и " + call.machine.describe(right));
}

Cell expectBitwise(Call& call)
{
    const Value& left = call.argument(0);
    const Value& right = call.argument(1);
    if (left.kind == ValueKind::Float && right.kind == ValueKind::Float)
    {
        uint64_t leftBits = 0;
        uint64_t rightBits = 0;
        std::memcpy(&leftBits, &left.floating, sizeof(leftBits));
        std::memcpy(&rightBits, &right.floating, sizeof(rightBits));
        if (leftBits == rightBits)
        {
            return call.machine.success(nullptr);
        }
        return testFailure(call, "ожидалось побитное совпадение: " + floatText(left.floating, left.number) + " и "
                + floatText(right.floating, right.number));
    }
    return expectEqual(call);
}

Cell expectClose(Call& call)
{
    const double left = floatOf(call, call.argument(0));
    const double right = floatOf(call, call.argument(1));
    const double tolerance = floatOf(call, call.argument(2));
    if (std::fabs(left - right) <= tolerance)
    {
        return call.machine.success(nullptr);
    }
    return testFailure(call, "ожидалась близость: " + floatText(left, Number::F64) + " и "
            + floatText(right, Number::F64) + " с допуском " + floatText(tolerance, Number::F64));
}

Cell expectSuccess(Call& call)
{
    const Value& result = call.argument(0);
    if (isResult(call, result, "Успех"))
    {
        return call.machine.success(result.items.empty() ? voidCell() : result.items.front());
    }
    return testFailure(call,
        "ожидался успех, а получена ошибка " + (result.items.empty() ? "" : call.machine.describe(result.items.front())));
}

Cell expectFailure(Call& call)
{
    const Value& result = call.argument(0);
    if (isResult(call, result, "Ошибка"))
    {
        return call.machine.success(result.items.empty() ? voidCell() : result.items.front());
    }
    return testFailure(call, "ожидалась ошибка, а получен успех");
}

Cell testSkip(Call& call)
{
    return call.machine.structValue(
        call.machine.failureType, {stringCell("пропуск: " + std::string(textView(call.argument(0))))});
}

Cell regionAllocate(Call& call)
{
    if (call.arguments.empty())
    {
        call.machine.panic("`выделить` без значения");
    }
    return referenceCell(copyCell(call.arguments.back()));
}

Cell regionAllocateSlice(Call& call)
{
    const size_t length = sizeArgument(call, call.arguments.size() - 1);
    std::vector<Cell> items;
    items.reserve(length);
    for (size_t index = 0; index < length; ++index)
    {
        items.push_back(voidCell());
    }
    return listCell(std::move(items), false);
}

Cell regionTryAllocate(Call& call)
{
    return call.machine.success(regionAllocate(call));
}

Cell arenaValue(Call& call, size_t capacity)
{
    return call.machine.structValue(
        call.machine.arenaType, {voidCell(), sizeCell(capacity), sizeCell(0)});
}

Cell arenaNew(Call& call)
{
    return arenaValue(call, sizeArgument(call, 0));
}

Cell arenaReset(Call& call)
{
    const Value& arena = *call.receiver;
    const Value& capacity = *dereference(arena.items.size() > 1 ? arena.items[1] : voidCell());
    return arenaValue(call, static_cast<size_t>(capacity.integer.magnitude));
}

Cell arenaRelease(Call&)
{
    return voidCell();
}

Cell memoryReplace(Call& call)
{
    const Cell& place = call.argumentCell(call.arguments.size() - 2);
    Cell previous = copyCell(place);
    *place = *copyCell(call.arguments.back());
    return previous;
}

Cell fileOpened(Call& call, const char* mode)
{
    const std::filesystem::path path = pathFromUtf8(textView(call.argument(0)));
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), mode[0] == 'r' ? L"rb" : L"wb");
#else
    std::FILE* file = std::fopen(path.c_str(), mode);
#endif
    if (file == nullptr)
    {
        std::error_code error;
        return inputError(call, std::filesystem::exists(path, error) || mode[0] == 'w' ? "НетДоступа" : "НеНайдено");
    }
    const int32_t descriptor = call.machine.nextDescriptor++;
    call.machine.files[descriptor] = file;
    return call.machine.success(call.machine.structValue(
        call.machine.fileType, {integerCell(signedInteger(descriptor), Number::I32)}));
}

std::FILE* fileOf(Call& call)
{
    const Value& handle = *call.receiver;
    const Value& descriptor = *dereference(handle.items.front());
    const auto found = call.machine.files.find(static_cast<int32_t>(integerBits(descriptor.integer, Number::I32)));
    if (found == call.machine.files.end())
    {
        call.machine.panic("файл уже закрыт");
    }
    return found->second;
}

Cell fileOpen(Call& call)
{
    return fileOpened(call, "rb");
}

Cell fileCreate(Call& call)
{
    return fileOpened(call, "wb");
}

Cell fileRead(Call& call)
{
    std::FILE* file = fileOf(call);
    const Cell& buffer = call.argumentCell(0);
    if (buffer->kind != ValueKind::List)
    {
        call.machine.panic("`читать` ждёт изменяемый срез байтов");
    }
    const size_t capacity = listLength(*buffer);
    std::string bytes(capacity, '\0');
    const size_t read = std::fread(bytes.data(), 1, capacity, file);
    if (read < capacity && std::ferror(file) != 0)
    {
        return inputError(call, "Отказано");
    }
    for (size_t index = 0; index < read; ++index)
    {
        *listElement(*buffer, index) = *integerCell(unsignedInteger(static_cast<unsigned char>(bytes[index])), Number::U8);
    }
    return call.machine.success(sizeCell(read));
}

Cell fileWrite(Call& call)
{
    std::FILE* file = fileOf(call);
    const std::string bytes = bytesOf(call, call.argument(0));
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        return inputError(call, "НетМеста");
    }
    return call.machine.success(nullptr);
}

Cell fileClose(Call& call)
{
    std::FILE* file = fileOf(call);
    const Value& descriptor = *dereference(call.receiver->items.front());
    call.machine.files.erase(static_cast<int32_t>(integerBits(descriptor.integer, Number::I32)));
    std::fclose(file);
    return voidCell();
}

const Value& numericArgument(Call& call)
{
    return call.argument(call.arguments.size() - 1);
}

Cell mathRoot(Call& call)
{
    const Value& value = numericArgument(call);
    return floatCell(std::sqrt(floatOf(call, value)), floatNumberOf(value));
}

Cell mathFloor(Call& call)
{
    const Value& value = numericArgument(call);
    if (value.kind == ValueKind::Integer)
    {
        return integerCell(value.integer, value.number);
    }
    return floatCell(std::floor(value.floating), floatNumberOf(value));
}

Cell mathAbsolute(Call& call)
{
    const Value& value = numericArgument(call);
    if (value.kind == ValueKind::Integer)
    {
        const WideInteger magnitude{false, value.integer.magnitude};
        if (!fits(magnitude, value.number))
        {
            call.machine.panic("переполнение `" + std::string(numberName(value.number)) + "` в `модуль`");
        }
        return integerCell(magnitude, value.number);
    }
    return floatCell(std::fabs(value.floating), floatNumberOf(value));
}

Cell mathClamp(Call& call)
{
    const size_t base = call.arguments.size() - 3;
    const Cell& value = call.argumentCell(base);
    const Cell& low = call.argumentCell(base + 1);
    const Cell& high = call.argumentCell(base + 2);
    if (value->kind == ValueKind::Float)
    {
        return floatCell(std::clamp(value->floating, floatOf(call, *low), floatOf(call, *high)), value->number);
    }
    if (compareIntegers(value->integer, low->integer) < 0)
    {
        return copyCell(low);
    }
    if (compareIntegers(value->integer, high->integer) > 0)
    {
        return copyCell(high);
    }
    return copyCell(value);
}

Cell mathFusedMultiplyAdd(Call& call)
{
    const size_t base = call.arguments.size() - 3;
    const Value& first = call.argument(base);
    const Number number = floatNumberOf(first);
    if (number == Number::F32)
    {
        const float result = std::fma(static_cast<float>(first.floating),
            static_cast<float>(floatOf(call, call.argument(base + 1))),
            static_cast<float>(floatOf(call, call.argument(base + 2))));
        return floatCell(result, number);
    }
    return floatCell(
        std::fma(first.floating, floatOf(call, call.argument(base + 1)), floatOf(call, call.argument(base + 2))),
        number);
}

}

void registerBuiltins(std::unordered_map<std::string, Builtin>& table)
{
    const std::pair<std::string_view, Builtin> entries[] = {
        {"Массив.новый", arrayNew},
        {"Массив.добавить", arrayAdd},
        {"Массив.количество", arrayCount},
        {"Массив.снять", arrayPop},
        {"Массив.удалитьПеренося", arraySwapRemove},
        {"Массив.итер", arrayIterate},
        {"Массив.перечислить", arrayEnumerate},
        {"Графемы.количество", graphemesCount},
        {"Графемы.след", graphemesNext},
        {"Графемы.взять", graphemesTake},
        {"Символы.след", charsNext},
        {"стр.байты", stringBytes},
        {"стр.срез", stringSlice},
        {"стр.графемы", stringGraphemes},
        {"стр.символы", stringChars},
        {"стр.изUtf8", stringFromUtf8},
        {"сим.изКода", charFromCode},
        {"в32.биты", float32Bits},
        {"в64.биты", float64Bits},
        {"в32.конечное", floatFinite},
        {"в64.конечное", floatFinite},
        {"ядро.ос.аргументы", systemArguments},
        {"ядро.фс.прочитатьТекст", readText},
        {"ядро.консоль.печать", consolePrint},
        {"ядро.консоль.ошибка", consoleError},
        {"ядро.журнал.сообщить", logInform},
        {"ядро.журнал.предупредить", logWarn},
        {"ядро.журнал.ошибка", logError},
        {"Построитель.новый", builderNew},
        {"Построитель.добавить", builderAdd},
        {"Построитель.добавитьСимвол", builderAddChar},
        {"Построитель.готово", builderDone},
        {"ядро.текст.формат", textFormat},
        {"ядро.числа.разобратьВ64", parseFloat},
        {"ядро.числа.записатьЦ64", writeInteger},
        {"ядро.числа.записатьВ64", writeFloat},
        {"ядро.тест.ожидать", expectTrue},
        {"ядро.тест.ожидатьРавно", expectEqual},
        {"ядро.тест.ожидатьПобитно", expectBitwise},
        {"ядро.тест.ожидатьБлизко", expectClose},
        {"ядро.тест.ожидатьУспех", expectSuccess},
        {"ядро.тест.ожидатьОшибку", expectFailure},
        {"ядро.тест.пропуск", testSkip},
        {"Регион.выделить", regionAllocate},
        {"Регион.выделитьСрез", regionAllocateSlice},
        {"Регион.попытатьсяВыделить", regionTryAllocate},
        {"ядро.память.новыйРегион", arenaNew},
        {"Рег.сбросить", arenaReset},
        {"Рег.освободить", arenaRelease},
        {"ядро.память.заменить", memoryReplace},
        {"Файл.открыть", fileOpen},
        {"Файл.создать", fileCreate},
        {"Файл.читать", fileRead},
        {"Файл.писать", fileWrite},
        {"Файл.закрыть", fileClose},
        {"ядро.математика.корень", mathRoot},
        {"ядро.математика.пол", mathFloor},
        {"ядро.математика.модуль", mathAbsolute},
        {"ядро.математика.ограничить", mathClamp},
        {"ядро.математика.умножитьСложить", mathFusedMultiplyAdd},
    };
    for (const auto& [name, builtin] : entries)
    {
        table[nameKey(name)] = builtin;
    }
}

}
