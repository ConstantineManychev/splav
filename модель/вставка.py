import ctypes
import ctypes.util
import glob
import os
import pathlib
import sys

СНИМКИ = pathlib.Path(__file__).resolve().parent / "заголовки"

ВРЕМЕННЫЙ_ФАЙЛ = "splav_vstavka.c"

ФЛАГИ_РАЗБОРА = 0x01 | 0x200

АРГУМЕНТЫ = ("-x", "c", "-std=c17", "-ferror-limit=0")

ИМЕНА_ПЛАТФОРМЫ = {
    "size_t": "разм",
    "ssize_t": "смещ",
    "ptrdiff_t": "смещ",
    "intptr_t": "смещ",
    "uintptr_t": "разм",
    "int8_t": "ц8",
    "int16_t": "ц16",
    "int32_t": "ц32",
    "int64_t": "ц64",
    "uint8_t": "н8",
    "uint16_t": "н16",
    "uint32_t": "н32",
    "uint64_t": "н64",
}


class ОшибкаВставки(Exception):
    pass


class Непереводимо(Exception):
    pass


class _Строка(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("flags", ctypes.c_uint)]


class _Курсор(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("xdata", ctypes.c_int), ("data", ctypes.c_void_p * 3)]


class _Тип(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("data", ctypes.c_void_p * 2)]


class _Место(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2), ("int_data", ctypes.c_uint)]


class _Диапазон(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2), ("begin_int_data", ctypes.c_uint), ("end_int_data", ctypes.c_uint)]


class _Лексема(ctypes.Structure):
    _fields_ = [("int_data", ctypes.c_uint * 4), ("ptr_data", ctypes.c_void_p)]


class _Файл(ctypes.Structure):
    _fields_ = [("Filename", ctypes.c_char_p), ("Contents", ctypes.c_char_p), ("Length", ctypes.c_ulong)]


_ПОСЕТИТЕЛЬ = ctypes.CFUNCTYPE(ctypes.c_int, _Курсор, _Курсор, ctypes.py_object)

_П = ctypes.POINTER

_ПРОТОТИПЫ = {
    "clang_createIndex": ([ctypes.c_int, ctypes.c_int], ctypes.c_void_p),
    "clang_disposeIndex": ([ctypes.c_void_p], None),
    "clang_parseTranslationUnit": (
        [ctypes.c_void_p, ctypes.c_char_p, _П(ctypes.c_char_p), ctypes.c_int, _П(_Файл), ctypes.c_uint, ctypes.c_uint],
        ctypes.c_void_p,
    ),
    "clang_disposeTranslationUnit": ([ctypes.c_void_p], None),
    "clang_getTranslationUnitCursor": ([ctypes.c_void_p], _Курсор),
    "clang_visitChildren": ([_Курсор, _ПОСЕТИТЕЛЬ, ctypes.py_object], ctypes.c_uint),
    "clang_getCursorKind": ([_Курсор], ctypes.c_int),
    "clang_getCursorSpelling": ([_Курсор], _Строка),
    "clang_getCursorType": ([_Курсор], _Тип),
    "clang_getCanonicalType": ([_Тип], _Тип),
    "clang_getPointeeType": ([_Тип], _Тип),
    "clang_getArrayElementType": ([_Тип], _Тип),
    "clang_getArraySize": ([_Тип], ctypes.c_longlong),
    "clang_getResultType": ([_Тип], _Тип),
    "clang_getNumArgTypes": ([_Тип], ctypes.c_int),
    "clang_getArgType": ([_Тип, ctypes.c_uint], _Тип),
    "clang_isFunctionTypeVariadic": ([_Тип], ctypes.c_uint),
    "clang_getTypedefDeclUnderlyingType": ([_Курсор], _Тип),
    "clang_getTypeDeclaration": ([_Тип], _Курсор),
    "clang_Type_getNamedType": ([_Тип], _Тип),
    "clang_Type_getModifiedType": ([_Тип], _Тип),
    "clang_Type_getNullability": ([_Тип], ctypes.c_int),
    "clang_Type_getSizeOf": ([_Тип], ctypes.c_longlong),
    "clang_getTypeSpelling": ([_Тип], _Строка),
    "clang_Cursor_isBitField": ([_Курсор], ctypes.c_uint),
    "clang_getFieldDeclBitWidth": ([_Курсор], ctypes.c_int),
    "clang_getEnumConstantDeclValue": ([_Курсор], ctypes.c_longlong),
    "clang_getEnumConstantDeclUnsignedValue": ([_Курсор], ctypes.c_ulonglong),
    "clang_getEnumDeclIntegerType": ([_Курсор], _Тип),
    "clang_Cursor_isAnonymous": ([_Курсор], ctypes.c_uint),
    "clang_isCursorDefinition": ([_Курсор], ctypes.c_uint),
    "clang_getCursorDefinition": ([_Курсор], _Курсор),
    "clang_Cursor_isNull": ([_Курсор], ctypes.c_int),
    "clang_equalCursors": ([_Курсор, _Курсор], ctypes.c_uint),
    "clang_getCursorLocation": ([_Курсор], _Место),
    "clang_getSpellingLocation": (
        [_Место, _П(ctypes.c_void_p), _П(ctypes.c_uint), _П(ctypes.c_uint), _П(ctypes.c_uint)],
        None,
    ),
    "clang_getExpansionLocation": (
        [_Место, _П(ctypes.c_void_p), _П(ctypes.c_uint), _П(ctypes.c_uint), _П(ctypes.c_uint)],
        None,
    ),
    "clang_getFileName": ([ctypes.c_void_p], _Строка),
    "clang_getCursorResultType": ([_Курсор], _Тип),
    "clang_Cursor_isMacroFunctionLike": ([_Курсор], ctypes.c_uint),
    "clang_Cursor_isMacroBuiltin": ([_Курсор], ctypes.c_uint),
    "clang_getCursorExtent": ([_Курсор], _Диапазон),
    "clang_getRangeEnd": ([_Диапазон], _Место),
    "clang_tokenize": ([ctypes.c_void_p, _Диапазон, _П(_П(_Лексема)), _П(ctypes.c_uint)], None),
    "clang_getTokenSpelling": ([ctypes.c_void_p, _Лексема], _Строка),
    "clang_getTokenLocation": ([ctypes.c_void_p, _Лексема], _Место),
    "clang_disposeTokens": ([ctypes.c_void_p, _П(_Лексема), ctypes.c_uint], None),
    "clang_Cursor_Evaluate": ([_Курсор], ctypes.c_void_p),
    "clang_EvalResult_getKind": ([ctypes.c_void_p], ctypes.c_int),
    "clang_EvalResult_getAsStr": ([ctypes.c_void_p], ctypes.c_char_p),
    "clang_EvalResult_getAsDouble": ([ctypes.c_void_p], ctypes.c_double),
    "clang_EvalResult_dispose": ([ctypes.c_void_p], None),
    "clang_getNumDiagnostics": ([ctypes.c_void_p], ctypes.c_uint),
    "clang_getDiagnostic": ([ctypes.c_void_p, ctypes.c_uint], ctypes.c_void_p),
    "clang_getDiagnosticSeverity": ([ctypes.c_void_p], ctypes.c_int),
    "clang_getDiagnosticLocation": ([ctypes.c_void_p], _Место),
    "clang_getDiagnosticSpelling": ([ctypes.c_void_p], _Строка),
    "clang_disposeDiagnostic": ([ctypes.c_void_p], None),
    "clang_getCString": ([_Строка], ctypes.c_char_p),
    "clang_disposeString": ([_Строка], None),
    "clang_getClangVersion": ([], _Строка),
}

ВИДЫ = {
    "структ": 2,
    "объединение": 3,
    "перечисление": 5,
    "поле": 6,
    "элемент": 7,
    "функция": 8,
    "переменная": 9,
    "параметр": 10,
    "псевдоним": 20,
    "макрос": 501,
}

ТИП = {
    "Invalid": 0, "Unexposed": 1, "Void": 2, "Bool": 3, "Char_U": 4, "UChar": 5, "UShort": 8, "UInt": 9,
    "ULong": 10, "ULongLong": 11, "UInt128": 12, "Char_S": 13, "SChar": 14, "WChar": 15, "Short": 16,
    "Int": 17, "Long": 18, "LongLong": 19, "Int128": 20, "Float": 21, "Double": 22, "Pointer": 101,
    "Record": 105, "Enum": 106, "Typedef": 107, "FunctionNoProto": 110, "FunctionProto": 111,
    "ConstantArray": 112, "IncompleteArray": 114, "Elaborated": 119, "Attributed": 163,
}

ЦЕЛЫЕ = {
    ТИП["Bool"]: "бул",
    ТИП["Char_U"]: "н8",
    ТИП["Char_S"]: "н8",
    ТИП["UChar"]: "н8",
    ТИП["SChar"]: "ц8",
    ТИП["Float"]: "в32",
    ТИП["Double"]: "в64",
    ТИП["Int128"]: "ц128",
    ТИП["UInt128"]: "н128",
}

ЗНАКОВЫЕ = {ТИП["Short"], ТИП["Int"], ТИП["Long"], ТИП["LongLong"], ТИП["WChar"]}

БЕЗЗНАКОВЫЕ = {ТИП["UShort"], ТИП["UInt"], ТИП["ULong"], ТИП["ULongLong"]}


class Clang:
    def __init__(self):
        self.библиотека, self.путь = self.найти()
        if self.библиотека is None:
            raise ОшибкаВставки("не найден libclang: укажите путь в LIBCLANG_PATH")
        for имя, (аргументы, итог) in _ПРОТОТИПЫ.items():
            функция = getattr(self.библиотека, имя)
            функция.argtypes = аргументы
            функция.restype = итог
        self.версия = self.строка(self.библиотека.clang_getClangVersion())

    @staticmethod
    def найти():
        кандидаты = []
        заданный = os.environ.get("LIBCLANG_PATH")
        if заданный:
            путь = pathlib.Path(заданный)
            if путь.is_dir():
                for шаблон in ("libclang*.so*", "libclang.dll", "libclang*.dylib"):
                    кандидаты += sorted(путь.glob(шаблон), reverse=True)
            else:
                кандидаты.append(путь)
        if sys.platform == "win32":
            программы = pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
            кандидаты += [программы / "LLVM" / "bin" / "libclang.dll", "libclang.dll"]
        elif sys.platform == "darwin":
            кандидаты += [
                "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib",
                "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib",
                "/opt/homebrew/opt/llvm/lib/libclang.dylib",
                "/usr/local/opt/llvm/lib/libclang.dylib",
            ]
        else:
            кандидаты += sorted(glob.glob("/usr/lib/llvm-*/lib/libclang*.so*"), reverse=True)
            кандидаты += sorted(glob.glob("/usr/lib/*/libclang-*.so*"), reverse=True)
            кандидаты += sorted(glob.glob("/usr/lib64/libclang*.so*"), reverse=True)
        найденное = ctypes.util.find_library("clang")
        if найденное:
            кандидаты.append(найденное)
        for кандидат in кандидаты:
            try:
                return ctypes.CDLL(str(кандидат)), str(кандидат)
            except OSError:
                continue
        return None, None

    def строка(self, значение):
        текст = self.библиотека.clang_getCString(значение)
        self.библиотека.clang_disposeString(значение)
        return текст.decode("utf-8", "replace") if текст else ""

    def разобрать(self, текст, аргументы=АРГУМЕНТЫ):
        индекс = self.библиотека.clang_createIndex(0, 0)
        содержимое = текст.encode("utf-8")
        файл = _Файл(ВРЕМЕННЫЙ_ФАЙЛ.encode(), содержимое, len(содержимое))
        массив = (ctypes.c_char_p * len(аргументы))(*[аргумент.encode() for аргумент in аргументы])
        единица = self.библиотека.clang_parseTranslationUnit(
            индекс, ВРЕМЕННЫЙ_ФАЙЛ.encode(), массив, len(аргументы), ctypes.byref(файл), 1, ФЛАГИ_РАЗБОРА,
        )
        if not единица:
            self.библиотека.clang_disposeIndex(индекс)
            raise ОшибкаВставки("libclang не разобрал заголовок")
        return Единица(self, индекс, единица)


class Единица:
    def __init__(self, clang, индекс, единица):
        self.clang = clang
        self.б = clang.библиотека
        self.индекс = индекс
        self.единица = единица

    def закрыть(self):
        self.б.clang_disposeTranslationUnit(self.единица)
        self.б.clang_disposeIndex(self.индекс)

    def корень(self):
        return self.б.clang_getTranslationUnitCursor(self.единица)

    def дети(self, курсор):
        список = []

        def посетить(ребёнок, родитель, данные):
            список.append(_Курсор.from_buffer_copy(ребёнок))
            return 1

        обратный = _ПОСЕТИТЕЛЬ(посетить)
        self.б.clang_visitChildren(курсор, обратный, ctypes.py_object(None))
        return список

    def имя(self, курсор):
        return self.clang.строка(self.б.clang_getCursorSpelling(курсор))

    def место(self, курсор=None, место=None, раскрытие=False):
        if место is None:
            место = self.б.clang_getCursorLocation(курсор)
        файл = ctypes.c_void_p()
        строка = ctypes.c_uint()
        столбец = ctypes.c_uint()
        смещение = ctypes.c_uint()
        способ = self.б.clang_getExpansionLocation if раскрытие else self.б.clang_getSpellingLocation
        способ(место, ctypes.byref(файл), ctypes.byref(строка), ctypes.byref(столбец), ctypes.byref(смещение))
        имя = self.clang.строка(self.б.clang_getFileName(файл)) if файл.value else None
        return имя, строка.value, столбец.value, смещение.value

    def ошибки(self):
        итог = []
        for номер in range(self.б.clang_getNumDiagnostics(self.единица)):
            диагностика = self.б.clang_getDiagnostic(self.единица, номер)
            if self.б.clang_getDiagnosticSeverity(диагностика) >= 3:
                файл, строка, _, _ = self.место(место=self.б.clang_getDiagnosticLocation(диагностика), раскрытие=True)
                текст = self.clang.строка(self.б.clang_getDiagnosticSpelling(диагностика))
                итог.append((файл, строка, текст))
            self.б.clang_disposeDiagnostic(диагностика)
        return итог

    def лексемы(self, курсор):
        протяжённость = self.б.clang_getCursorExtent(курсор)
        конец = self.место(место=self.б.clang_getRangeEnd(протяжённость))
        указатель = _П(_Лексема)()
        число = ctypes.c_uint()
        self.б.clang_tokenize(self.единица, протяжённость, ctypes.byref(указатель), ctypes.byref(число))
        итог = []
        for номер in range(число.value):
            лексема = указатель[номер]
            место = self.место(место=self.б.clang_getTokenLocation(self.единица, лексема))
            if место[0] == конец[0] and место[3] >= конец[3] and итог:
                break
            итог.append(self.clang.строка(self.б.clang_getTokenSpelling(self.единица, лексема)))
        if число.value:
            self.б.clang_disposeTokens(self.единица, указатель, число)
        return итог

    def вычислить(self, курсор):
        результат = self.б.clang_Cursor_Evaluate(курсор)
        if not результат:
            return None
        try:
            вид = self.б.clang_EvalResult_getKind(результат)
            if вид == 4:
                значение = self.б.clang_EvalResult_getAsStr(результат)
                return ("строка", значение.decode("utf-8", "replace") if значение is not None else "")
            if вид == 2:
                return ("вещественное", self.б.clang_EvalResult_getAsDouble(результат))
            return None
        finally:
            self.б.clang_EvalResult_dispose(результат)


class Образ:
    def __init__(self, заголовок, текст, источник, ключи, пропущено):
        self.заголовок = заголовок
        self.текст = текст
        self.источник = источник
        self.ключи = ключи
        self.пропущено = пропущено


class Импорт:
    def __init__(self, clang, заголовок):
        self.clang = clang
        self.заголовок = заголовок
        self.е = clang.разобрать(f"#include <{заголовок}>\n")
        self.б = self.е.б
        отказы = [ошибка for ошибка in self.е.ошибки() if ошибка[0] == ВРЕМЕННЫЙ_ФАЙЛ or "not found" in ошибка[2]]
        if отказы:
            self.е.закрыть()
            raise ОшибкаВставки(f"заголовок {заголовок} не разобран: {отказы[0][2]}")
        self.путь = None
        self.записи = {}
        self.имена_записей = {}
        self.псевдонимы = {}
        self.перечисления = {}
        self.функции = {}
        self.переменные = {}
        self.константы = {}
        self.макросы = []
        self.пропущено = []
        self.ключи = {}
        self.обычные = set()
        self.строки = []

    def ключ(self, курсор):
        файл, строка, столбец, _ = self.е.место(курсор)
        return (файл, строка, столбец)

    def собрать(self):
        for курсор in self.е.дети(self.е.корень()):
            вид = курсор.kind
            файл = self.е.место(курсор)[0]
            if файл is None:
                continue
            if файл.endswith(self.заголовок.replace("/", os.sep)) and self.путь is None:
                self.путь = файл
            имя = self.е.имя(курсор)
            if вид in (ВИДЫ["структ"], ВИДЫ["объединение"]):
                self.запись(курсор)
            elif вид == ВИДЫ["перечисление"]:
                self.перечисление(курсор)
            elif вид == ВИДЫ["псевдоним"]:
                self.псевдонимы.setdefault(имя, курсор)
                self.обычные.add(имя)
            elif вид == ВИДЫ["функция"]:
                self.функции.setdefault(имя, курсор)
                self.обычные.add(имя)
            elif вид == ВИДЫ["переменная"]:
                self.переменные.setdefault(имя, курсор)
                self.обычные.add(имя)
            elif вид == ВИДЫ["макрос"]:
                if not self.б.clang_Cursor_isMacroBuiltin(курсор):
                    self.макросы.append(курсор)
        for курсор in self.е.дети(self.е.корень()):
            if курсор.kind in (ВИДЫ["структ"], ВИДЫ["объединение"]) and self.е.место(курсор)[0] is not None:
                for ребёнок in self.е.дети(курсор):
                    if ребёнок.kind in (ВИДЫ["структ"], ВИДЫ["объединение"]):
                        self.запись(ребёнок)
                    elif ребёнок.kind == ВИДЫ["перечисление"]:
                        self.перечисление(ребёнок)

    def запись(self, курсор):
        определение = self.б.clang_getCursorDefinition(курсор)
        if not self.б.clang_Cursor_isNull(определение):
            курсор = определение
        ключ = self.ключ(курсор)
        if ключ not in self.записи:
            self.записи[ключ] = курсор
        return ключ

    def перечисление(self, курсор):
        ключ = self.ключ(курсор)
        if ключ in self.перечисления:
            return
        self.перечисления[ключ] = курсор
        for элемент in self.е.дети(курсор):
            if элемент.kind == ВИДЫ["элемент"]:
                имя = self.е.имя(элемент)
                self.обычные.add(имя)
                тип_целого = self.б.clang_getEnumDeclIntegerType(курсор)
                if self.без_знака(тип_целого):
                    значение = self.б.clang_getEnumConstantDeclUnsignedValue(элемент)
                else:
                    значение = self.б.clang_getEnumConstantDeclValue(элемент)
                self.константы.setdefault(имя, (тип_целого, значение))

    def без_знака(self, тип):
        канон = self.б.clang_getCanonicalType(тип)
        return канон.kind in БЕЗЗНАКОВЫЕ or канон.kind in (ТИП["UChar"], ТИП["Char_U"], ТИП["UInt128"], ТИП["Bool"])

    def назвать_записи(self):
        кандидаты = {}
        for имя, курсор in self.псевдонимы.items():
            тип = self.развернуть(self.б.clang_getTypedefDeclUnderlyingType(курсор))
            if тип.kind == ТИП["Record"]:
                ключ = self.запись(self.запись_типа(тип))
                кандидаты.setdefault(ключ, []).append((имя.startswith("_"), self.ключ(курсор), имя))
        self.теги = {}
        for ключ, курсор in self.записи.items():
            тег = self.е.имя(курсор)
            if self.б.clang_Cursor_isAnonymous(курсор) or not тег or " " in тег:
                тег = None
            if ключ in кандидаты:
                имя = sorted(кандидаты[ключ])[0][2]
                if тег is not None and тег != имя and тег not in self.обычные:
                    self.теги[тег] = имя
            elif тег is not None and тег in self.обычные:
                имя = ("union_" if курсор.kind == ВИДЫ["объединение"] else "struct_") + тег
            else:
                имя = тег
            self.имена_записей[ключ] = имя

    def запись_типа(self, тип):
        курсор = self.б.clang_getTypeDeclaration(тип)
        определение = self.б.clang_getCursorDefinition(курсор)
        return курсор if self.б.clang_Cursor_isNull(определение) else определение

    def развернуть(self, тип):
        while тип.kind in (ТИП["Elaborated"], ТИП["Attributed"]):
            if тип.kind == ТИП["Elaborated"]:
                тип = self.б.clang_Type_getNamedType(тип)
            else:
                тип = self.б.clang_Type_getModifiedType(тип)
        return тип

    def тип(self, тип, параметр=False):
        непустой = False
        while тип.kind in (ТИП["Elaborated"], ТИП["Attributed"]):
            if тип.kind == ТИП["Attributed"]:
                if self.б.clang_Type_getNullability(тип) == 0:
                    непустой = True
                тип = self.б.clang_Type_getModifiedType(тип)
            else:
                тип = self.б.clang_Type_getNamedType(тип)
        вид = тип.kind
        if вид == ТИП["Typedef"]:
            курсор = self.б.clang_getTypeDeclaration(тип)
            имя = self.е.имя(курсор)
            if имя in ИМЕНА_ПЛАТФОРМЫ:
                return ИМЕНА_ПЛАТФОРМЫ[имя]
            if self.е.место(курсор)[0] is None:
                return self.тип(self.б.clang_getCanonicalType(тип), параметр)
            self.перевести_псевдоним(имя)
            if имя in self.готовые_псевдонимы:
                return имя
            return self.тип(self.б.clang_getTypedefDeclUnderlyingType(курсор), параметр)
        if вид == ТИП["Pointer"]:
            цель = self.б.clang_getPointeeType(тип)
            канон = self.развернуть(self.б.clang_getCanonicalType(цель))
            if канон.kind in (ТИП["FunctionProto"], ТИП["FunctionNoProto"]):
                return ("" if непустой else "?") + self.тип(цель)
            if канон.kind == ТИП["Void"]:
                return ("*" if непустой else "?*") + "пусто"
            return ("*" if непустой else "?*") + self.тип(цель)
        if вид == ТИП["Record"]:
            курсор = self.запись_типа(тип)
            if self.е.место(курсор)[0] is None:
                raise Непереводимо(f"встроенная запись {self.е.имя(курсор)}")
            ключ = self.запись(курсор)
            имя = self.имена_записей.get(ключ)
            if имя is None:
                raise Непереводимо("безымянная запись")
            return имя
        if вид == ТИП["Enum"]:
            курсор = self.б.clang_getTypeDeclaration(тип)
            return self.тип(self.б.clang_getEnumDeclIntegerType(курсор))
        if вид == ТИП["ConstantArray"]:
            элемент = self.б.clang_getArrayElementType(тип)
            if параметр:
                return "?*" + self.тип(элемент)
            return f"[{self.б.clang_getArraySize(тип)}]{self.тип(элемент)}"
        if вид == ТИП["IncompleteArray"]:
            элемент = self.б.clang_getArrayElementType(тип)
            if параметр:
                return "?*" + self.тип(элемент)
            return f"[0]{self.тип(элемент)}"
        if вид in (ТИП["FunctionProto"], ТИП["FunctionNoProto"]):
            return self.тип_функции(тип)
        if вид == ТИП["Void"]:
            return "пусто"
        if вид in ЦЕЛЫЕ:
            return ЦЕЛЫЕ[вид]
        if вид in ЗНАКОВЫЕ or вид in БЕЗЗНАКОВЫЕ:
            размер = self.б.clang_Type_getSizeOf(тип) * 8
            return ("н" if вид in БЕЗЗНАКОВЫЕ else "ц") + str(размер)
        if вид == ТИП["Unexposed"]:
            канон = self.б.clang_getCanonicalType(тип)
            if канон.kind != ТИП["Unexposed"]:
                return self.тип(канон, параметр)
        raise Непереводимо(f"тип {self.clang.строка(self.б.clang_getTypeSpelling(тип))}")

    def тип_функции(self, тип):
        параметры = []
        if тип.kind == ТИП["FunctionProto"]:
            for номер in range(self.б.clang_getNumArgTypes(тип)):
                параметры.append(self.тип(self.б.clang_getArgType(тип, номер), параметр=True))
            if self.б.clang_isFunctionTypeVariadic(тип):
                параметры.append("...")
        итог = self.тип(self.б.clang_getResultType(тип))
        хвост = "" if итог == "пусто" else f" -> {итог}"
        return f"внеш \"C\" функ({', '.join(параметры)}){хвост}"

    def перевести_псевдоним(self, имя):
        if имя in self.готовые_псевдонимы or имя in self.в_работе or имя in self.отвергнутые:
            return
        курсор = self.псевдонимы.get(имя)
        if курсор is None:
            return
        self.в_работе.add(имя)
        try:
            основа = self.б.clang_getTypedefDeclUnderlyingType(курсор)
            развёрнутый = self.развернуть(основа)
            if развёрнутый.kind == ТИП["Record"]:
                ключ = self.ключ(self.запись_типа(развёрнутый))
                if self.имена_записей.get(ключ) == имя:
                    self.готовые_псевдонимы[имя] = None
                    return
            значение = self.тип(основа)
            self.готовые_псевдонимы[имя] = значение
        except Непереводимо as причина:
            self.отвергнутые.add(имя)
            self.пропущено.append(("псевдоним", имя, str(причина)))
        finally:
            self.в_работе.discard(имя)

    def построить(self, закреплённые=()):
        self.собрать()
        self.назвать_записи()
        self.готовые_псевдонимы = {}
        self.в_работе = set()
        self.отвергнутые = set()
        for имя in sorted(self.псевдонимы):
            if имя not in ИМЕНА_ПЛАТФОРМЫ:
                self.перевести_псевдоним(имя)
        блоки = []
        for имя in sorted(self.готовые_псевдонимы):
            значение = self.готовые_псевдонимы[имя]
            if значение is not None:
                блоки.append(f"откр конст {имя} = {значение}")
                self.ключи[имя] = self.ключ(self.псевдонимы[имя])
        for тег in sorted(self.теги):
            if тег not in self.готовые_псевдонимы:
                блоки.append(f"откр конст {тег} = {self.теги[тег]}")
        записи = sorted(self.записи.items(), key=lambda пара: str(self.имена_записей.get(пара[0])))
        for ключ, курсор in записи:
            имя = self.имена_записей.get(ключ)
            if имя is None:
                continue
            блоки.append(self.текст_записи(имя, курсор))
            self.ключи[имя] = ключ
        for имя in sorted(self.константы):
            тип, значение = self.константы[имя]
            try:
                блоки.append(f"откр конст {имя}: {self.тип(тип)} = {значение}")
            except Непереводимо as причина:
                self.пропущено.append(("константа", имя, str(причина)))
        for имя in sorted(self.функции):
            текст = self.текст_функции(имя, self.функции[имя])
            if текст is not None:
                блоки.append(текст)
        for имя in sorted(self.переменные):
            try:
                тип = self.тип(self.б.clang_getCursorType(self.переменные[имя]))
                блоки.append(f"[ПеременнаяСи]\nоткр конст {имя}: {тип} = {self.ноль(тип)}")
            except Непереводимо as причина:
                self.пропущено.append(("переменная", имя, str(причина)))
        блоки += self.текст_макросов()
        self.е.закрыть()
        return "\n\n".join(блоки) + "\n"

    def ноль(self, тип):
        if тип.startswith("?"):
            return "Ничего"
        if тип.startswith("в"):
            return "0.0"
        if тип == "бул":
            return "нет"
        return "0"

    def текст_записи(self, имя, курсор):
        атрибуты = ["[Си]"]
        if курсор.kind == ВИДЫ["объединение"]:
            атрибуты.append("[Объединение]")
        строки = []
        полная = self.б.clang_isCursorDefinition(курсор)
        if полная:
            try:
                for поле in self.е.дети(курсор):
                    if поле.kind != ВИДЫ["поле"]:
                        continue
                    имя_поля = self.е.имя(поле)
                    if not имя_поля or self.б.clang_Cursor_isAnonymous(поле):
                        raise Непереводимо("безымянное поле")
                    тип = self.тип(self.б.clang_getCursorType(поле))
                    приставка = ""
                    if self.б.clang_Cursor_isBitField(поле):
                        приставка = f"[Биты({self.б.clang_getFieldDeclBitWidth(поле)})]\n    "
                    строки.append(f"    {приставка}откр {имя_поля}: {тип}")
            except Непереводимо as причина:
                self.пропущено.append(("поля", имя, str(причина)))
                строки = []
                полная = False
        тело = "\n".join(строки)
        текст = "\n".join(атрибуты) + f"\nоткр структ {имя}\n{{\n" + (тело + "\n" if тело else "") + "}"
        if полная:
            текст += f"\n\n[Встроено]\nоткр функ {имя}.ноль() -> {имя}"
        return текст

    def текст_функции(self, имя, курсор):
        try:
            тип = self.б.clang_getCursorType(курсор)
            параметры = []
            объявленные = [ребёнок for ребёнок in self.е.дети(курсор) if ребёнок.kind == ВИДЫ["параметр"]]
            if тип.kind == ТИП["FunctionProto"]:
                число = self.б.clang_getNumArgTypes(тип)
                for номер in range(число):
                    тип_параметра = self.б.clang_getArgType(тип, номер)
                    имя_параметра = f"п{номер + 1}"
                    if len(объявленные) == число:
                        имя_параметра = self.е.имя(объявленные[номер]) or имя_параметра
                        тип_параметра = self.б.clang_getCursorType(объявленные[номер])
                    параметры.append(f"{имя_параметра}: {self.тип(тип_параметра, параметр=True)}")
                if self.б.clang_isFunctionTypeVariadic(тип):
                    параметры.append("...")
            итог = self.тип_результата(курсор)
        except Непереводимо as причина:
            self.пропущено.append(("функция", имя, str(причина)))
            return None
        хвост = "" if итог == "пусто" else f" -> {итог}"
        self.ключи[имя] = self.ключ(курсор)
        return f"откр внеш \"C\" функ {имя}({', '.join(параметры)}){хвост}"

    def тип_результата(self, курсор):
        тип = self.б.clang_getCursorResultType(курсор)
        if тип.kind in (ТИП["Elaborated"], ТИП["Typedef"]):
            return self.тип(тип)
        имя = self.е.имя(курсор)
        лексемы = self.е.лексемы(курсор)
        if имя in лексемы:
            звёзды = 0
            for лексема in reversed(лексемы[:лексемы.index(имя)]):
                if лексема == "*":
                    звёзды += 1
                    continue
                if лексема in ИМЕНА_ПЛАТФОРМЫ or лексема in self.псевдонимы:
                    основа = self.б.clang_getCanonicalType(тип)
                    for _ in range(звёзды):
                        основа = self.б.clang_getCanonicalType(self.б.clang_getPointeeType(основа))
                    курсор_псевдонима = self.псевдонимы.get(лексема)
                    if курсор_псевдонима is not None:
                        записанный = self.б.clang_getCanonicalType(self.б.clang_getTypedefDeclUnderlyingType(курсор_псевдонима))
                        if self.clang.строка(self.б.clang_getTypeSpelling(записанный)) != self.clang.строка(
                                self.б.clang_getTypeSpelling(основа)):
                            break
                        self.перевести_псевдоним(лексема)
                        if лексема in ИМЕНА_ПЛАТФОРМЫ:
                            база = ИМЕНА_ПЛАТФОРМЫ[лексема]
                        elif лексема in self.готовые_псевдонимы:
                            база = лексема
                        else:
                            break
                        return "?*" * звёзды + база
                break
        return self.тип(тип)

    def текст_макросов(self):
        объектные = []
        функциональные = []
        for курсор in self.макросы:
            имя = self.е.имя(курсор)
            if имя in self.обычные or имя in self.готовые_псевдонимы:
                continue
            лексемы = self.е.лексемы(курсор)
            if not лексемы or лексемы[0] != имя:
                continue
            if self.б.clang_Cursor_isMacroFunctionLike(курсор):
                функциональные.append((имя, лексемы[1:]))
            elif len(лексемы) > 1 and self.сбалансированы(лексемы[1:]):
                объектные.append(имя)
        блоки = self.пробы(объектные)
        for имя, лексемы in функциональные:
            текст = self.пересылка(имя, лексемы)
            if текст is not None:
                блоки.append(текст)
            else:
                self.пропущено.append(("макрос", имя, "функциональный макрос не сводится к вызову функции"))
        return блоки

    @staticmethod
    def сбалансированы(лексемы):
        глубина = 0
        for лексема in лексемы:
            if лексема in ("{", "}", ";", "#", "##"):
                return False
            if лексема in ("(", "["):
                глубина += 1
            elif лексема in (")", "]"):
                глубина -= 1
                if глубина < 0:
                    return False
        return глубина == 0

    def пробы(self, имена):
        if not имена:
            return []
        строки = [f"#include <{self.заголовок}>"]
        номера = {}
        for номер, имя in enumerate(имена):
            начало = len(строки) + 1
            номера[номер] = начало
            строки += [
                f"__typeof__(({имя})) splav_t_{номер};",
                f"enum {{ splav_e_{номер} = ({имя}) }};",
                f"__typeof__(&({имя})) splav_l_{номер};",
                f"static const char splav_s_{номер}[] = {имя};",
                f"static const double splav_d_{номер} = ({имя});",
            ]
        единица = self.clang.разобрать("\n".join(строки) + "\n")
        плохие = {строка for файл, строка, _ in единица.ошибки() if файл == ВРЕМЕННЫЙ_ФАЙЛ}
        найденные = {}

        def собрать(курсор):
            for ребёнок in единица.дети(курсор):
                имя = единица.имя(ребёнок)
                if ребёнок.kind == ВИДЫ["перечисление"]:
                    собрать(ребёнок)
                elif имя.startswith("splav_"):
                    найденные[имя] = ребёнок

        собрать(единица.корень())
        блоки = []
        for номер, имя in enumerate(имена):
            начало = номера[номер]
            годен = [строка not in плохие for строка in range(начало, начало + 5)]
            if not годен[0] or f"splav_t_{номер}" not in найденные:
                continue
            сырой = единица.б.clang_getCursorType(найденные[f"splav_t_{номер}"])
            канон = единица.б.clang_getCanonicalType(сырой)
            try:
                if годен[1] and f"splav_e_{номер}" in найденные and канон.kind in ЗНАКОВЫЕ | БЕЗЗНАКОВЫЕ | {
                    ТИП["Char_S"], ТИП["Char_U"], ТИП["UChar"], ТИП["SChar"], ТИП["Bool"]
                }:
                    элемент = найденные[f"splav_e_{номер}"]
                    if канон.kind in БЕЗЗНАКОВЫЕ:
                        значение = единица.б.clang_getEnumConstantDeclUnsignedValue(элемент)
                    else:
                        значение = единица.б.clang_getEnumConstantDeclValue(элемент)
                    блоки.append(f"откр конст {имя}: {self.тип_пробы(единица, канон)} = {значение}")
                    continue
                if канон.kind == ТИП["ConstantArray"] and годен[3] and f"splav_s_{номер}" in найденные:
                    значение = единица.вычислить(найденные[f"splav_s_{номер}"])
                    if значение is not None and значение[0] == "строка":
                        блоки.append(f"откр конст {имя}: [:0]н8 = {self.литерал_строки(значение[1])}")
                        continue
                if канон.kind in (ТИП["Float"], ТИП["Double"]) and годен[4] and f"splav_d_{номер}" in найденные:
                    значение = единица.вычислить(найденные[f"splav_d_{номер}"])
                    if значение is not None and значение[0] == "вещественное":
                        запись = repr(float(значение[1]))
                        if запись[0].isdigit() or запись[0] == "-":
                            блоки.append(f"откр конст {имя}: {self.тип_пробы(единица, канон)} = {запись}")
                            continue
                if годен[2]:
                    тип = self.тип_пробы(единица, канон)
                    блоки.append(f"[ПеременнаяСи]\nоткр конст {имя}: {тип} = {self.ноль(тип)}")
                    continue
            except Непереводимо as причина:
                self.пропущено.append(("макрос", имя, str(причина)))
                continue
            self.пропущено.append(("макрос", имя, "не константа и не переменная C"))
        единица.закрыть()
        return блоки

    def тип_пробы(self, единица, тип):
        прежняя = self.е
        self.е = единица
        self.б = единица.б
        try:
            return self.тип(тип)
        finally:
            self.е = прежняя
            self.б = прежняя.б

    @staticmethod
    def литерал_строки(текст):
        замены = {"\\": "\\\\", "\"": "\\\"", "\n": "\\n", "\t": "\\t", "\r": "\\r", "\0": "\\0"}
        итог = []
        for знак in текст:
            if знак in замены:
                итог.append(замены[знак])
            elif ord(знак) < 0x20:
                итог.append(f"\\u{{{ord(знак):x}}}")
            else:
                итог.append(знак)
        return "\"" + "".join(итог) + "\""

    def пересылка(self, имя, лексемы):
        if not лексемы or лексемы[0] != "(":
            return None
        конец = лексемы.index(")")
        параметры = [лексема for лексема in лексемы[1:конец] if лексема != ","]
        тело = лексемы[конец + 1:]
        while len(тело) >= 2 and тело[0] == "(" and self.парная(тело, 0) == len(тело) - 1:
            тело = тело[1:-1]
        if len(тело) < 3 or тело[0] not in self.функции or тело[1] != "(" or self.парная(тело, 1) != len(тело) - 1:
            return None
        цель = self.функции[тело[0]]
        аргументы = self.разделить(тело[2:-1])
        тип = self.б.clang_getCursorType(цель)
        if тип.kind != ТИП["FunctionProto"] or len(аргументы) != self.б.clang_getNumArgTypes(тип):
            return None
        соответствие = {}
        for номер, аргумент in enumerate(аргументы):
            while len(аргумент) >= 2 and аргумент[0] == "(" and self.парная(аргумент, 0) == len(аргумент) - 1:
                аргумент = аргумент[1:-1]
            if len(аргумент) == 1 and аргумент[0] in параметры:
                if аргумент[0] in соответствие:
                    return None
                соответствие[аргумент[0]] = номер
            elif any(лексема in параметры for лексема in аргумент):
                return None
        if set(соответствие) != set(параметры):
            return None
        try:
            части = [
                f"{параметр}: {self.тип(self.б.clang_getArgType(тип, соответствие[параметр]), параметр=True)}"
                for параметр in параметры
            ]
            итог = self.тип(self.б.clang_getResultType(тип))
        except Непереводимо:
            return None
        хвост = "" if итог == "пусто" else f" -> {итог}"
        return f"[Встроено]\nоткр функ {имя}({', '.join(части)}){хвост}"

    @staticmethod
    def парная(лексемы, начало):
        глубина = 0
        for номер in range(начало, len(лексемы)):
            if лексемы[номер] == "(":
                глубина += 1
            elif лексемы[номер] == ")":
                глубина -= 1
                if глубина == 0:
                    return номер
        return -1

    @staticmethod
    def разделить(лексемы):
        части = [[]]
        глубина = 0
        for лексема in лексемы:
            if лексема == "," and глубина == 0:
                части.append([])
                continue
            if лексема in ("(", "["):
                глубина += 1
            elif лексема in (")", "]"):
                глубина -= 1
            части[-1].append(лексема)
        return части if части != [[]] else []


_CLANG = None

ИСТОЧНИКИ = {}


def clang():
    global _CLANG
    if _CLANG is None:
        _CLANG = Clang()
    return _CLANG


def снимок(заголовок):
    return СНИМКИ / f"{pathlib.PurePosixPath(заголовок).stem}.спл"


def образ(заголовок, свежий=True):
    причина = None
    if свежий:
        try:
            импорт = Импорт(clang(), заголовок)
            текст = импорт.построить()
            источник = f"{clang().версия}, {импорт.путь or заголовок}"
            ИСТОЧНИКИ[заголовок] = источник
            return Образ(заголовок, текст, источник, импорт.ключи, импорт.пропущено)
        except ОшибкаВставки as ошибка:
            причина = str(ошибка)
    путь = снимок(заголовок)
    if not путь.exists():
        raise ОшибкаВставки(причина or f"нет снимка {путь.name}")
    источник = f"снимок модель/заголовки/{путь.name}" + (f": {причина}" if причина else "")
    ИСТОЧНИКИ[заголовок] = источник
    return Образ(заголовок, путь.read_text(encoding="utf-8"), источник, {}, [])


def обновить_снимки(заголовки):
    for заголовок in заголовки:
        результат = образ(заголовок)
        снимок(заголовок).write_text(результат.текст, encoding="utf-8")
        print(f"{заголовок}: {результат.источник}, {результат.текст.count(chr(10))} строк, пропущено {len(результат.пропущено)}")


if __name__ == "__main__":
    обновить_снимки(sys.argv[1:] or ["stdio.h", "string.h", "errno.h", "zlib.h"])
