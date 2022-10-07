#include "Cpp2CASTConsumer.hh"
#include "ASTUtils.hh"
#include "DeclStmtTypeLoc.hh"
#include "DeclCollectorMatchHandler.hh"
#include "ExpansionMatchHandler.hh"
#include "AlignmentMatchers.hh"
#include "Properties.hh"
#include "IncludeCollector.hh"

#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

#include <algorithm>
#include <functional>
#include <set>

#include "assert.h"

// TODO:    Remember to check if we should be ignoring implicit casts
//          and if we should be using TK_IgnoreUnlessSpelledInSource
//          and ignoringImplicit
// NOTE:    We can't use TK_IgnoreUnlessSpelledInSource because it ignores
//          paren exprs

namespace cpp2c
{

    template <typename T>
    inline void printIfIsOneOf(const clang::Stmt *ST)
    {
        if (clang::isa<T>(ST))
            llvm::errs() << typeid(T).name() << ",";
    }

    template <typename T1, typename T2, typename... Ts>
    inline void printIfIsOneOf(const clang::Stmt *ST)
    {
        if (clang::isa<T1>(ST))
            llvm::errs() << typeid(T1).name() << ",";
        printIfIsOneOf<T2, Ts...>(ST);
    }

    inline std::function<bool(const clang::Stmt *)>
    stmtIsBinOp(clang::BinaryOperator::Opcode OC)
    {
        return [OC](const clang::Stmt *ST)
        {
            if (auto BO = clang::dyn_cast<clang::BinaryOperator>(ST))
                return BO->getOpcode() == OC;
            return false;
        };
    }

    // Returns true if the given type represent an anonymous type
    inline bool containsAnonymousType(const clang::Type *T)
    {
        assert(T);
        return (T->getAsTagDecl() &&
                T->getAsTagDecl()->getName().empty()) ||
               ((T->isAnyPointerType() || T->isArrayType()) &&
                containsAnonymousType(T->getPointeeOrArrayElementType()));
    }

    // Returns true if the the given stmt is a valid function argument
    bool isValidArgument(const clang::Stmt *ST)
    {
        // The stmt exists
        if (!ST)
            return false;
        // The stmt is an expr
        if (!clang::isa<clang::Expr>(ST))
            return false;
        auto E = clang::dyn_cast<clang::Expr>(ST);
        auto T = E->getType().getTypePtrOrNull();
        // Type must exist
        if (!T)
            return false;
        // Type is not void
        if (T->isVoidType())
            return false;
        // Type does not contain an anonymous type
        if (containsAnonymousType(T))
            return false;

        return true;
    }

    // Expects that the passed expansion is entirely wellformed
    bool hasValidSignature(MacroExpansionNode *Expansion)
    {
        assert(Expansion);

        // Verify that the expansion expands to a stmt
        if (!Expansion->AlignedRoot->ST)
            return false;

        auto ST = Expansion->AlignedRoot->ST;

        assert(Expansion->MI);
        // If the expansion is to an object-like macro, then check the
        // entire expansion as if it were a function argument
        if (Expansion->MI->isObjectLike())
            return isValidArgument(ST);

        // If the expansion is function-like...
        // ...then the return type must not contain an anonymous type...
        if (auto E = clang::dyn_cast<clang::Expr>(ST))
        {
            auto T = E->getType().getTypePtrOrNull();
            if (!T || containsAnonymousType(T))
                return false;
        }

        // ...and all the arguments must be valid
        for (auto &&Arg : Expansion->Arguments)
            if (Arg.AlignedRoots.empty() ||
                !(Arg.AlignedRoots.front().ST) ||
                !isValidArgument(Arg.AlignedRoots.front().ST))
                return false;

        return true;
    }

    Cpp2CASTConsumer::Cpp2CASTConsumer(clang::CompilerInstance &CI)
    {
        clang::Preprocessor &PP = CI.getPreprocessor();
        clang::ASTContext &Ctx = CI.getASTContext();

        MF = new cpp2c::MacroForest(PP, Ctx);
        IC = new cpp2c::IncludeCollector();

        PP.addPPCallbacks(std::unique_ptr<cpp2c::MacroForest>(MF));
        PP.addPPCallbacks(std::unique_ptr<cpp2c::IncludeCollector>(IC));
    }

    void Cpp2CASTConsumer::HandleTranslationUnit(clang::ASTContext &Ctx)
    {
        auto &SM = Ctx.getSourceManager();
        auto &LO = Ctx.getLangOpts();

        // Collect declaration ranges
        std::vector<const clang::Decl *> Decls = ({
            MatchFinder Finder;
            DeclCollectorMatchHandler Handler;
            auto Matcher = decl(unless(anyOf(
                                    isImplicit(),
                                    translationUnitDecl())))
                               .bind("root");
            Finder.addMatcher(Matcher, &Handler);
            Finder.matchAST(Ctx);
            Handler.Decls;
        });

        // Dump include-directive information
        {
            std::set<llvm::StringRef> NonGlobalIncludes;
            for (auto &&Item : IC->IncludeEntriesLocs)
            {
                auto FE = Item.first;
                auto HashLoc = Item.second;

                llvm::errs() << "#include,";

                // TODO: Would be really nice to have a monad here...
                auto IncludedInFID = SM.getFileID(HashLoc);
                bool valid = IncludedInFID.isValid();
                if (valid)
                {
                    llvm::errs() << "IncludedInFID,";
                    auto IncludedInFE = SM.getFileEntryForID(IncludedInFID);
                    valid = IncludedInFE != nullptr;
                    if (valid)
                    {
                        llvm::errs() << "IncludedInFE,";
                        auto IncludedInRealpath = IncludedInFE->tryGetRealPathName();
                        valid = !IncludedInRealpath.empty();
                        if (valid)
                        {
                            llvm::errs() << "IncludedInRealpath,";
                            auto IncludedFileRealpath = FE->tryGetRealPathName();
                            valid = !IncludedFileRealpath.empty();
                            if (valid)
                            {
                                llvm::errs() << "IncludedFileRealpath,";
                                valid =
                                    NonGlobalIncludes.find(IncludedInRealpath) ==
                                    NonGlobalIncludes.end();
                                if (valid)
                                {
                                    llvm::errs() << "Not included in a non-globally included file,";
                                    valid = !std::any_of(
                                        Decls.begin(),
                                        Decls.end(),
                                        [&Item, &SM, &LO](const clang::Decl *D)
                                        {
                                            auto Range =
                                                clang::SourceRange(
                                                    SM.getFileLoc(D->getBeginLoc()),
                                                    SM.getFileLoc(D->getEndLoc()));
                                            // Include the location just after the declaration
                                            // to account for semicolons.
                                            // If the decl does not have semicolon after it,
                                            // that's fine since it would be a non-global
                                            // location anyway
                                            if (auto Tok = clang::Lexer::findNextToken(
                                                               Range.getEnd(), SM, LO)
                                                               .getPointer())
                                                Range.setEnd(SM.getFileLoc(Tok->getEndLoc()));
                                            auto L = SM.getFileLoc(Item.second);
                                            return Range.fullyContains(L);
                                        });
                                    if (!valid)
                                        NonGlobalIncludes.insert(IncludedFileRealpath);
                                }
                            }
                        }
                    }
                }
                llvm::errs() << (valid ? "Included at global scope,"
                                       : "Included at non-global scope,")
                             << "\n";
            }
        }

        // Dump macro expansion information

        for (auto TLE : MF->TopLevelExpansions)
        {
            using namespace clang::ast_matchers;

            //// Find potential AST roots of the entire expansion

            // Match stmts
            if (!(TLE)->DefinitionTokens.empty())
            {
                MatchFinder Finder;
                ExpansionMatchHandler Handler;
                auto Matcher = stmt(unless(anyOf(implicitCastExpr(),
                                                 implicitValueInitExpr())),
                                    alignsWithExpansion(&Ctx, TLE))
                                   .bind("root");
                Finder.addMatcher(Matcher, &Handler);
                Finder.matchAST(Ctx);
                for (auto M : Handler.Matches)
                    TLE->ASTRoots.push_back(M);
            }

            // Match decls
            if (!(TLE->DefinitionTokens.empty()))
            {
                MatchFinder Finder;
                ExpansionMatchHandler Handler;
                auto Matcher = decl(alignsWithExpansion(&Ctx, TLE))
                                   .bind("root");
                Finder.addMatcher(Matcher, &Handler);
                Finder.matchAST(Ctx);
                for (auto M : Handler.Matches)
                    TLE->ASTRoots.push_back(M);
            }

            // Match type locs
            if (!((TLE)->DefinitionTokens.empty()))
            {
                MatchFinder Finder;
                ExpansionMatchHandler Handler;
                auto Matcher = typeLoc(alignsWithExpansion(&Ctx, (TLE)))
                                   .bind("root");
                Finder.addMatcher(Matcher, &Handler);
                Finder.matchAST(Ctx);
                for (auto M : Handler.Matches)
                    TLE->ASTRoots.push_back(M);
            }

            // If the expansion only aligns with one node, then set this
            // as its aligned root
            if (TLE->ASTRoots.size() == 1)
                TLE->AlignedRoot = &(TLE->ASTRoots.front());

            //// Find AST roots aligned with each of the expansion's arguments

            for (auto &&Arg : TLE->Arguments)
            {
                // Match stmts
                if (!Arg.Tokens.empty())
                {
                    MatchFinder Finder;
                    ExpansionMatchHandler Handler;
                    auto Matcher = stmt(unless(anyOf(implicitCastExpr(),
                                                     implicitValueInitExpr())),
                                        isSpelledFromTokens(&Ctx, Arg.Tokens))
                                       .bind("root");
                    Finder.addMatcher(Matcher, &Handler);
                    Finder.matchAST(Ctx);
                    for (auto M : Handler.Matches)
                        Arg.AlignedRoots.push_back(M);
                }

                // Match decls
                if (!(Arg.Tokens.empty()))
                {
                    MatchFinder Finder;
                    ExpansionMatchHandler Handler;
                    auto Matcher = decl(isSpelledFromTokens(&Ctx, Arg.Tokens))
                                       .bind("root");
                    Finder.addMatcher(Matcher, &Handler);
                    Finder.matchAST(Ctx);
                    for (auto M : Handler.Matches)
                        Arg.AlignedRoots.push_back(M);
                }

                // Match type locs
                if (!(Arg.Tokens.empty()))
                {
                    MatchFinder Finder;
                    ExpansionMatchHandler Handler;
                    auto Matcher =
                        typeLoc(isSpelledFromTokens(&Ctx, Arg.Tokens))
                            .bind("root");
                    Finder.addMatcher(Matcher, &Handler);
                    Finder.matchAST(Ctx);
                    for (auto M : Handler.Matches)
                        Arg.AlignedRoots.push_back(M);
                }
            }

            //// Print macro info

            // TLE->dumpMacroInfo(llvm::errs());

            // TLE->dumpASTInfo(llvm::errs(),
            //                  Ctx.getSourceManager(), Ctx.getLangOpts());

            if (TLE->MI->isObjectLike())
                llvm::errs() << "Object-like,";
            else
                llvm::errs() << "Function-like,";

            if (TLE->ASTRoots.empty())
                llvm::errs() << "No aligned body,";
            else if (TLE->ASTRoots.size() > 1)
                llvm::errs() << "Multiple aligned bodies,";
            else
            {
                llvm::errs() << "Single aligned body,";
                auto D = TLE->AlignedRoot->D;
                auto ST = TLE->AlignedRoot->ST;
                auto TL = TLE->AlignedRoot->TL;

                if (ST)
                {

                    llvm::errs() << "Stmt,";

                    printIfIsOneOf<clang::DoStmt,
                                   clang::ContinueStmt,
                                   clang::BreakStmt,
                                   clang::ReturnStmt,
                                   clang::GotoStmt,

                                   clang::Expr,
                                   clang::CharacterLiteral,
                                   clang::IntegerLiteral,
                                   clang::FloatingLiteral,
                                   clang::FixedPointLiteral,
                                   clang::ImaginaryLiteral,
                                   clang::StringLiteral,
                                   clang::CompoundLiteralExpr>(ST);

                    if (isInTree(ST, stmtIsA<clang::ConditionalOperator>()))
                        llvm::errs() << "ConditionalOperator,";
                    if (isInTree(ST,
                                 stmtIsBinOp(
                                     clang::BinaryOperator::Opcode::BO_LAnd)))
                        llvm::errs() << "BinaryOperator::Opcode::BO_LAnd,";
                    if (isInTree(ST,
                                 stmtIsBinOp(
                                     clang::BinaryOperator::Opcode::BO_LOr)))
                        llvm::errs() << "BinaryOperator::Opcode::BO_LOr,";

                    if (hasValidSignature(TLE))
                        llvm::errs() << "Valid signature,";
                    else
                        llvm::errs() << "Invalid signature,";
                }

                if (D)
                    llvm::errs() << "Decl,";

                if (TL)
                    llvm::errs() << "TypeLoc,";
            }

            // Check that the number of AST nodes aligned with each argument
            // equals the number of times that argument was expanded
            if (TLE->Arguments.empty())
            {
                llvm::errs() << "No arguments,";
            }
            else
            {
                if (std::all_of(TLE->Arguments.begin(),
                                TLE->Arguments.end(),
                                [](MacroExpansionArgument Arg)
                                { return Arg.AlignedRoots.size() ==
                                         Arg.numberOfTimesExpanded; }))
                    llvm::errs() << "Aligned arguments,";
                else
                    llvm::errs() << "Unaligned arguments,";
            }

            // Check for semantic properties of interface-equivalence
            // TODO: Check for these properties in decls as well?

            if (TLE->AlignedRoot &&
                TLE->AlignedRoot->ST)
            {
                if (isHygienic(Ctx, TLE))
                    llvm::errs() << "Hygienic,";
                else
                    llvm::errs() << "Unhygienic,";
                if (isParameterSideEffectFree(Ctx, TLE))
                    llvm::errs() << "Parameter side-effect free,";
                else
                    llvm::errs() << "Parameter side-effects,";
                if (isLValueIndependent(Ctx, TLE))
                    llvm::errs() << "L-value independent,";
                else
                    llvm::errs() << "L-value dependent,";
            }

            llvm::errs() << "\n";

            delete TLE;
        }
    }
} // namespace cpp2c
