#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

// обработкa случая невиртуального деструктора
void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {
    // 1 позиция символа '~' (начало имени деструктора)
    SourceLocation TildeLoc = Dtor->getLocation();
    if (TildeLoc.isInvalid() || TildeLoc.isMacroID())
        return;

    // 2 по заданию: проверяем, что это основной файл
    if (!SM.isWrittenInMainFile(TildeLoc))
        return;

    // 3 защита от дублей: один и тот же деструктор может матчиться много раз
    SourceLocation FileLoc = SM.getFileLoc(TildeLoc);
    unsigned Key = SM.getFileOffset(FileLoc);

    if (!virtualDtorLocations.insert(Key).second)
        return;

    // 4 вставляем "virtual " перед '~'
    Rewrite.InsertTextBefore(TildeLoc, "virtual ");

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлен деструктор");
    Diag.Report(Dtor->getLocation(), DiagID);
}

// обработкa случая отсутствие override
void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    const auto &LangOpts = Method->getASTContext().getLangOpts();

    // 1 стартуем с конца имени метода
    SourceLocation Loc = Method->getNameInfo().getEndLoc();
    if (Loc.isInvalid() || Loc.isMacroID())
        return;

    // 2 сдвигаемся в позицию "после имени"
    Loc = clang::Lexer::getLocForEndOfToken(Loc, 0, SM, LangOpts);
    if (Loc.isInvalid() || Loc.isMacroID())
        return;

    // 3 сканируем токены вперёд до закрывающей ')'
    clang::Token Tok;
    SourceLocation InsertLoc;

    while (true) {
        if (clang::Lexer::getRawToken(Loc, Tok, SM, LangOpts, /*IgnoreWhiteSpace=*/true))
            return;

        if (Tok.is(clang::tok::r_paren)) {
            InsertLoc = clang::Lexer::getLocForEndOfToken(Tok.getLocation(), 0, SM, LangOpts);
            break;
        }

        // шаг к следующему токену
        Loc = clang::Lexer::getLocForEndOfToken(Tok.getLocation(), 0, SM, LangOpts);
        if (Loc.isInvalid() || Loc.isMacroID())
            return;
    }

    if (InsertLoc.isInvalid() || InsertLoc.isMacroID())
        return;

    // 4 вставляем override сразу после ')'
    Rewrite.InsertText(InsertLoc, " override", /*InsertAfter=*/true, /*IndentNewLines=*/false);

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлен метод");
    Diag.Report(Method->getLocation(), DiagID);
}

// обработкa случая отсутствие & в range-for
void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлена переменная");
    Diag.Report(LoopVar->getLocation(), DiagID);

    // 1 получаем информацию о том, как тип записан в исходнике
    const TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI)
        return;

    TypeLoc TL = TSI->getTypeLoc();
    if (TL.isNull())
        return;

    // 2 конец участка текста, описывающего тип
    SourceLocation TypeEnd = TL.getEndLoc();
    if (TypeEnd.isInvalid() || TypeEnd.isMacroID())
        return;

    // 3 позиция сразу после последнего токена типа
    const LangOptions &LangOpts = LoopVar->getASTContext().getLangOpts();
    SourceLocation InsertLoc = clang::Lexer::getLocForEndOfToken(TypeEnd, 0, SM, LangOpts);

    if (InsertLoc.isInvalid() || InsertLoc.isMacroID())
        return;

    // 4 вставляем '&' после типа
    Rewrite.InsertText(InsertLoc, "&", /*InsertAfter=*/true, /*IndentNewLines=*/false);
}

// матчеры для поиска невиртуальных деструкторов
auto NvDtorMatcher() {
    // 1 поиск объявлений CXXRecordDecl (class / struct), только полных определений
    // 2 ограничение на наличие хотя бы одного базового класса (derived-класс)
    // 3 переход к описанию базового класса (CXXBaseSpecifier)
    // 4 получение типа базового класса и проверка, что это тоже CXXRecordDecl
    // 5 поиск внутри базового класса объявления деструктора
    // 6 добавлены narrowing-фильтры: деструктор не virtual и не implicit
    // 7 исключены деструкторы, объявленные в системных заголовках
    // 8 найденному деструктору присвоено имя - в выводе clang-query он будет помечен как "nonVirtualDtor" (binds here)
    return cxxRecordDecl(isDefinition(),
                         hasAnyBase(cxxBaseSpecifier(hasType(cxxRecordDecl(hasMethod(
                             cxxDestructorDecl(unless(isVirtual()), unless(isImplicit()), isExpansionInMainFile())
                                 .bind("nonVirtualDtor")))))));
}

// матчеры для поиска методов без override
auto NoOverrideMatcher() {
    // 1 поиск узла - метод
    // 2 переопределяет базовый метод Но не имеет атрибута Override
    return cxxMethodDecl(isOverride(), unless(hasAttr(attr::Override)), isExpansionInMainFile(),
                         unless(cxxConstructorDecl()), unless(cxxDestructorDecl()), unless(isImplicit()))
        .bind("missingOverride");
}

// матчеры для поиска range-for без &
auto NoRefConstVarInRangeLoopMatcher() {
    // 1 поиск узлов CXXForRangeStmt (range-based for)
    // 2 поиск переменной цикла (loop variable)
    // 3 фильтрация: тип переменной должен быть const-qualified
    // 4 исключение случаев, когда тип уже является ссылкой (T&, const T&)
    // 5 исключение фундаментальных типов (int, char, double и т.п.)
    // 6 ограничение анализом только пользовательского кода
    // 7 привязка найденной переменной цикла под именем "loopVar"
    return cxxForRangeStmt(isExpansionInMainFile(),
                           hasLoopVariable(varDecl(hasType(qualType(isConstQualified(), unless(referenceType()),
                                                                    unless(pointerType()), unless(builtinType()))))
                                               .bind("loopVar")));
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) { Finder.matchAST(Context); }

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}

int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}