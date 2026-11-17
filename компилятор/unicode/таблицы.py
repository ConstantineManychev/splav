import argparse
import hashlib
import pathlib
import sys
import urllib.request

ВЕРСИЯ = "17.0.0"
ЗЕРКАЛО = "https://raw.githubusercontent.com/unicode-org/unicodetools/main/unicodetools/data"
ФАЙЛЫ = {
    "UnicodeData.txt": ("ucd/{версия}/UnicodeData.txt", "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c"),
    "DerivedCoreProperties.txt": (
        "ucd/{версия}/DerivedCoreProperties.txt",
        "24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08",
    ),
    "Scripts.txt": ("ucd/{версия}/Scripts.txt", "9f5e50d3abaee7d6ce09480f325c706f485ae3240912527e651954d2d6b035bf"),
    "DerivedNormalizationProps.txt": (
        "ucd/{версия}/DerivedNormalizationProps.txt",
        "71fd6a206a2c0cdd41feb6b7f656aa31091db45e9cedc926985d718397f9e488",
    ),
    "NormalizationTest.txt": (
        "ucd/{версия}/NormalizationTest.txt",
        "5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db",
    ),
    "confusables.txt": (
        "security/{версия}/confusables.txt",
        "091c7f82fc39ef208faf8f94d29c244de99254675e09de163160c810d13ef22a",
    ),
    "GraphemeBreakProperty.txt": (
        "ucd/{версия}/auxiliary/GraphemeBreakProperty.txt",
        "d6b51d1d2ae5c33b451b7ed994b48f1f4dc62b2272a5831e7fd418514a6bae89",
    ),
    "GraphemeBreakTest.txt": (
        "ucd/{версия}/auxiliary/GraphemeBreakTest.txt",
        "e2d134d2c52919bace503ebb6a551c1855fe1a1faec18478c78fff254a1793ec",
    ),
    "emoji-data.txt": (
        "ucd/{версия}/emoji/emoji-data.txt",
        "2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b",
    ),
}

КОРЕНЬ = pathlib.Path(__file__).resolve().parent.parent.parent
КЭШ = КОРЕНЬ / "сборка" / "ucd" / ВЕРСИЯ
ТАБЛИЦЫ_CPP = КОРЕНЬ / "компилятор" / "unicode" / "tables.inc"
ПРИМЕРЫ_CPP = КОРЕНЬ / "компилятор" / "tests" / "unicode_vectors.inc"
ТАБЛИЦЫ_PYTHON = КОРЕНЬ / "грамматика" / "юникод.py"

БУКВА = 1
ЗНАК = 2
ЛАТИНИЦА = 4
КИРИЛЛИЦА = 8
НАСЛЕДУЕМАЯ = 16
ДРУГАЯ = 32
СТРОЧНАЯ = 64
ЗАГЛАВНАЯ = 128
ИМЕНА_ФЛАГОВ = {
    БУКВА: "LETTER", ЗНАК: "MARK", ЛАТИНИЦА: "LATIN", КИРИЛЛИЦА: "CYRILLIC", НАСЛЕДУЕМАЯ: "INHERITED",
    ДРУГАЯ: "OTHER", СТРОЧНАЯ: "LOWER", ЗАГЛАВНАЯ: "UPPER",
}
ПОСЛЕДНЯЯ_ТОЧКА = 0x10FFFF
КЛАССЫ_ГРАФЕМ = {
    "CR": "CR", "LF": "LF", "Control": "Control", "Extend": "Extend", "ZWJ": "ZWJ",
    "Regional_Indicator": "RegionalIndicator", "Prepend": "Prepend", "SpacingMark": "SpacingMark",
    "L": "L", "V": "V", "T": "T", "LV": "LV", "LVT": "LVT",
}
ПИКТОГРАММА = 1
СОГЛАСНАЯ = 2
ПРОДОЛЖЕНИЕ = 4
СВЯЗКА = 8
ИМЕНА_ПРИЗНАКОВ_ГРАФЕМ = {
    ПИКТОГРАММА: "PICTOGRAPHIC", СОГЛАСНАЯ: "CONJUNCT_CONSONANT", ПРОДОЛЖЕНИЕ: "CONJUNCT_EXTEND", СВЯЗКА: "CONJUNCT_LINKER",
}
ПРИЗНАКИ_СВЯЗКИ = {"Consonant": СОГЛАСНАЯ, "Extend": ПРОДОЛЖЕНИЕ, "Linker": СВЯЗКА}


def сумма(данные):
    return hashlib.sha256(данные).hexdigest()


def загрузить(имя, каталог):
    относительный, ожидаемая = ФАЙЛЫ[имя]
    путь = каталог / имя
    if not путь.exists():
        каталог.mkdir(parents=True, exist_ok=True)
        адрес = f"{ЗЕРКАЛО}/{относительный.format(версия=ВЕРСИЯ)}"
        with urllib.request.urlopen(адрес) as ответ:
            путь.write_bytes(ответ.read())
    данные = путь.read_bytes()
    if сумма(данные) != ожидаемая:
        sys.exit(f"{путь}: sha256 {сумма(данные)} не совпадает с закреплённой {ожидаемая}")
    return данные.decode("utf-8")


def строки_данных(текст):
    for строка in текст.splitlines():
        содержимое = строка.split("#", 1)[0].strip()
        if содержимое:
            yield [поле.strip() for поле in содержимое.split(";")]


def диапазон(запись):
    if ".." in запись:
        начало, конец = запись.split("..")
        return int(начало, 16), int(конец, 16)
    return int(запись, 16), int(запись, 16)


def свойства(текст, нужные):
    итог = {имя: set() for имя in нужные}
    for поля in строки_данных(текст):
        if len(поля) >= 2 and поля[1] in итог:
            начало, конец = диапазон(поля[0])
            итог[поля[1]].update(range(начало, конец + 1))
    return итог


def данные_символов(текст):
    категории = {}
    классы = {}
    разложения = {}
    начало_диапазона = None
    for поля in строки_данных(текст):
        точка = int(поля[0], 16)
        if поля[1].endswith(", First>"):
            начало_диапазона = точка
            continue
        if поля[1].endswith(", Last>"):
            for внутри in range(начало_диапазона, точка + 1):
                категории[внутри] = поля[2]
            начало_диапазона = None
            continue
        категории[точка] = поля[2]
        if поля[3] != "0":
            классы[точка] = int(поля[3])
        if поля[5] and not поля[5].startswith("<"):
            разложения[точка] = [int(часть, 16) for часть in поля[5].split()]
    return категории, классы, разложения


def сжать(точки, значение):
    итог = []
    for точка in sorted(точки):
        текущее = значение(точка)
        if итог and итог[-1][1] == точка - 1 and итог[-1][2] == текущее:
            итог[-1][1] = точка
        else:
            итог.append([точка, точка, текущее])
    return итог


def таблица_имён(категории, ядро, письменности):
    флаги = {}
    for точка, категория in категории.items():
        if категория.startswith("L") and точка in ядро["XID_Start"]:
            if точка in письменности["Latin"]:
                флаги[точка] = БУКВА | ЛАТИНИЦА
            elif точка in письменности["Cyrillic"]:
                флаги[точка] = БУКВА | КИРИЛЛИЦА
            else:
                флаги[точка] = ДРУГАЯ
        elif категория in ("Mn", "Mc") and точка in ядро["XID_Continue"]:
            if точка in письменности["Latin"]:
                флаги[точка] = ЗНАК | ЛАТИНИЦА
            elif точка in письменности["Cyrillic"]:
                флаги[точка] = ЗНАК | КИРИЛЛИЦА
            elif точка in письменности["Inherited"] or точка in письменности["Common"]:
                флаги[точка] = ЗНАК | НАСЛЕДУЕМАЯ
            else:
                флаги[точка] = ЗНАК | ДРУГАЯ
        else:
            continue
        if точка in ядро["Lowercase"]:
            флаги[точка] |= СТРОЧНАЯ
        if точка in ядро["Uppercase"]:
            флаги[точка] |= ЗАГЛАВНАЯ
    return сжать(флаги, флаги.get)


def таблица_сочетаний(разложения, исключения):
    return sorted(
        (части[0], части[1], точка)
        for точка, части in разложения.items()
        if len(части) == 2 and точка not in исключения
    )


def таблица_смешения(текст):
    записи = []
    цели = []
    for поля in строки_данных(текст):
        источник = int(поля[0], 16)
        цель = [int(часть, 16) for часть in поля[1].split()]
        записи.append((источник, len(цели), len(цель)))
        цели += цель
    return sorted(записи), цели


def таблица_графем(разрывы, эмодзи, ядро):
    классы = {}
    for поля in строки_данных(разрывы):
        начало, конец = диапазон(поля[0])
        for точка in range(начало, конец + 1):
            классы[точка] = КЛАССЫ_ГРАФЕМ[поля[1]]
    признаки = {}
    for точка in свойства(эмодзи, ["Extended_Pictographic"])["Extended_Pictographic"]:
        признаки[точка] = ПИКТОГРАММА
    for поля in строки_данных(ядро):
        if len(поля) >= 3 and поля[1] == "InCB":
            начало, конец = диапазон(поля[0])
            for точка in range(начало, конец + 1):
                признаки[точка] = признаки.get(точка, 0) | ПРИЗНАКИ_СВЯЗКИ[поля[2]]
    точки = set(классы) | set(признаки)
    return сжать(точки, lambda точка: (классы.get(точка, "Other"), признаки.get(точка, 0)))


def имя_признаков(значение):
    return " | ".join(имя for бит, имя in ИМЕНА_ПРИЗНАКОВ_ГРАФЕМ.items() if значение & бит) or "0"


def шестнадцатеричное(точка):
    return f"0x{точка:04X}"


def имя_флагов(значение):
    return " | ".join(имя for бит, имя in ИМЕНА_ФЛАГОВ.items() if значение & бит)


def массив(заголовок, записи, в_строке):
    строки = [f"constexpr {заголовок} = {{"]
    for начало in range(0, len(записи), в_строке):
        строки.append("    " + " ".join(f"{запись}," for запись in записи[начало:начало + в_строке]))
    строки.append("};")
    return "\n".join(строки)


def фигурные(*значения):
    return "{" + ", ".join(значения) + "}"


def текст_cpp(имена, классы, разложения, сочетания, игнорируемые, смешение, цели, графемы):
    части = [
        f'constexpr std::string_view UNICODE_VERSION = "{ВЕРСИЯ}";',
        массив(
            "IdentifierRange IDENTIFIER_RANGES[]",
            [фигурные(шестнадцатеричное(а), шестнадцатеричное(б), имя_флагов(ф)) for а, б, ф in имена],
            1,
        ),
        массив(
            "CombiningClassRange COMBINING_CLASS_RANGES[]",
            [фигурные(шестнадцатеричное(а), шестнадцатеричное(б), str(к)) for а, б, к in сжать(классы, классы.get)],
            4,
        ),
        массив(
            "Decomposition DECOMPOSITIONS[]",
            [
                фигурные(шестнадцатеричное(точка), *(шестнадцатеричное(часть) for часть in (части_ + [0])[:2]))
                for точка, части_ in sorted(разложения.items())
            ],
            4,
        ),
        массив(
            "Composition COMPOSITIONS[]",
            [фигурные(*(шестнадцатеричное(точка) for точка in тройка)) for тройка in сочетания],
            4,
        ),
        массив(
            "CodePointRange DEFAULT_IGNORABLE_RANGES[]",
            [фигурные(шестнадцатеричное(а), шестнадцатеричное(б)) for а, б, _ in сжать(игнорируемые, lambda _: 0)],
            6,
        ),
        массив(
            "Confusable CONFUSABLES[]",
            [фигурные(шестнадцатеричное(источник), str(смещение), str(длина)) for источник, смещение, длина in смешение],
            6,
        ),
        массив("char32_t CONFUSABLE_TARGETS[]", [шестнадцатеричное(точка) for точка in цели], 12),
        массив(
            "GraphemeRange GRAPHEME_RANGES[]",
            [
                фигурные(шестнадцатеричное(а), шестнадцатеричное(б), f"GraphemeBreak::{класс}", имя_признаков(признаки))
                for а, б, (класс, признаки) in графемы
            ],
            1,
        ),
    ]
    return "\n\n".join(части) + "\n"


def литерал(точки):
    return 'U"' + "".join(f"\\x{точка:X}" for точка in точки) + '"'


def примеры_нормализации(текст, имена):
    допустимые = set()
    for начало, конец, _ in имена:
        допустимые.update(range(начало, конец + 1))
    допустимые.update(range(0x30, 0x3A))
    допустимые.add(0x5F)
    итог = []
    for номер, поля in enumerate(строки_данных(текст)):
        if поля[0].startswith("@"):
            continue
        столбцы = [[int(часть, 16) for часть in поле.split()] for поле in поля[:5]]
        источник, составная, разложенная = столбцы[0], столбцы[1], столбцы[2]
        именной = all(точка in допустимые for точка in источник)
        if (именной and номер % 3 == 0) or номер % 97 == 0:
            итог.append((источник, составная, разложенная))
    return итог


def примеры_графем(текст):
    итог = []
    for строка in текст.splitlines():
        содержимое = строка.split("#", 1)[0].split()
        if not содержимое:
            continue
        точки = [int(часть, 16) for часть in содержимое[1::2]]
        разрывы = "".join("1" if знак == "÷" else "0" for знак in содержимое[2:-1:2])
        итог.append((точки, разрывы))
    return итог


def текст_примеров(примеры, графемы):
    записи = [фигурные(литерал(а), литерал(б), литерал(в)) for а, б, в in примеры]
    разбиения = [фигурные(литерал(точки) + "sv", f'"{разрывы}"') for точки, разрывы in графемы]
    return (
        массив("NormalizationVector NORMALIZATION_VECTORS[]", записи, 1)
        + "\n\n"
        + массив("GraphemeVector GRAPHEME_VECTORS[]", разбиения, 1)
        + "\n"
    )


def текст_python(имена):
    строки = [
        f'ВЕРСИЯ = "{ВЕРСИЯ}"',
        f"БУКВА = {БУКВА}",
        f"ЗНАК = {ЗНАК}",
        f"ЛАТИНИЦА = {ЛАТИНИЦА}",
        f"КИРИЛЛИЦА = {КИРИЛЛИЦА}",
        f"НАСЛЕДУЕМАЯ = {НАСЛЕДУЕМАЯ}",
        f"ДРУГАЯ = {ДРУГАЯ}",
        f"СТРОЧНАЯ = {СТРОЧНАЯ}",
        f"ЗАГЛАВНАЯ = {ЗАГЛАВНАЯ}",
        "ПИСЬМЕННОСТИ = ЛАТИНИЦА | КИРИЛЛИЦА | НАСЛЕДУЕМАЯ | ДРУГАЯ",
        "",
        "ДИАПАЗОНЫ = (",
    ]
    for начало in range(0, len(имена), 6):
        строки.append("    " + " ".join(f"(0x{а:04X}, 0x{б:04X}, {ф})," for а, б, ф in имена[начало:начало + 6]))
    строки += [
        ")",
        "НАЧАЛА = tuple(запись[0] for запись in ДИАПАЗОНЫ)",
        "",
        "",
        "def флаги(символ):",
        "    import bisect",
        "",
        "    точка = ord(символ)",
        "    номер = bisect.bisect_right(НАЧАЛА, точка) - 1",
        "    if номер < 0 or точка > ДИАПАЗОНЫ[номер][1]:",
        "        return 0",
        "    return ДИАПАЗОНЫ[номер][2]",
        "",
    ]
    return "\n".join(строки)


def главная():
    разбор = argparse.ArgumentParser(description=f"Таблицы Юникода {ВЕРСИЯ} для лексера и прототипа")
    разбор.add_argument("--ucd", type=pathlib.Path, default=КЭШ, help="каталог с файлами UCD; недостающие скачиваются")
    разбор.add_argument("--проверить", action="store_true", help="не писать файлы, а сверить их с результатом")
    аргументы = разбор.parse_args()

    тексты = {имя: загрузить(имя, аргументы.ucd) for имя in ФАЙЛЫ}
    категории, классы, разложения = данные_символов(тексты["UnicodeData.txt"])
    ядро = свойства(
        тексты["DerivedCoreProperties.txt"],
        ["XID_Start", "XID_Continue", "Lowercase", "Uppercase", "Default_Ignorable_Code_Point"],
    )
    письменности = свойства(тексты["Scripts.txt"], ["Latin", "Cyrillic", "Inherited", "Common"])
    исключения = свойства(тексты["DerivedNormalizationProps.txt"], ["Full_Composition_Exclusion"])
    имена = таблица_имён(категории, ядро, письменности)
    сочетания = таблица_сочетаний(разложения, исключения["Full_Composition_Exclusion"])
    смешение, цели = таблица_смешения(тексты["confusables.txt"])
    графемы = таблица_графем(
        тексты["GraphemeBreakProperty.txt"], тексты["emoji-data.txt"], тексты["DerivedCoreProperties.txt"],
    )
    результаты = {
        ТАБЛИЦЫ_CPP: текст_cpp(
            имена, классы, разложения, сочетания, ядро["Default_Ignorable_Code_Point"], смешение, цели, графемы,
        ),
        ПРИМЕРЫ_CPP: текст_примеров(
            примеры_нормализации(тексты["NormalizationTest.txt"], имена),
            примеры_графем(тексты["GraphemeBreakTest.txt"]),
        ),
        ТАБЛИЦЫ_PYTHON: текст_python(имена),
    }
    расхождения = 0
    for путь, текст in результаты.items():
        if аргументы.проверить:
            совпало = путь.exists() and путь.read_text(encoding="utf-8") == текст
            расхождения += not совпало
            print(f"{'✓' if совпало else '✗'} {путь.relative_to(КОРЕНЬ)}")
        else:
            путь.write_text(текст, encoding="utf-8")
            print(f"{путь.relative_to(КОРЕНЬ)}: {текст.count(chr(10))} строк")
    return расхождения == 0


if __name__ == "__main__":
    sys.exit(0 if главная() else 1)
