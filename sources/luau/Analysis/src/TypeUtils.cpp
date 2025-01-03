// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/TypeUtils.h"

#include "Luau/Common.h"
#include "Luau/Normalize.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/TypeInfer.h"

#include <algorithm>

LUAU_FASTFLAG(LuauSolverV2);
LUAU_FASTFLAG(LuauAutocompleteRefactorsForIncrementalAutocomplete);

namespace Luau
{

bool inConditional(const TypeContext& context)
{
    return context == TypeContext::Condition;
}

bool occursCheck(TypeId needle, TypeId haystack)
{
    LUAU_ASSERT(get<BlockedType>(needle) || get<PendingExpansionType>(needle));
    haystack = follow(haystack);

    auto checkHaystack = [needle](TypeId haystack)
    {
        return occursCheck(needle, haystack);
    };

    if (needle == haystack)
        return true;
    else if (auto ut = get<UnionType>(haystack))
        return std::any_of(begin(ut), end(ut), checkHaystack);
    else if (auto it = get<IntersectionType>(haystack))
        return std::any_of(begin(it), end(it), checkHaystack);

    return false;
}

// FIXME: Property is quite large.
//
// Returning it on the stack like this isn't great. We'd like to just return a
// const Property*, but we mint a property of type any if the subject type is
// any.
std::optional<Property> findTableProperty(NotNull<BuiltinTypes> builtinTypes, ErrorVec& errors, TypeId ty, const std::string& name, Location location)
{
    if (get<AnyType>(ty))
        return Property::rw(ty);

    if (const TableType* tableType = getTableType(ty))
    {
        const auto& it = tableType->props.find(name);
        if (it != tableType->props.end())
            return it->second;
    }

    std::optional<TypeId> mtIndex = findMetatableEntry(builtinTypes, errors, ty, "__index", location);
    int count = 0;
    while (mtIndex)
    {
        TypeId index = follow(*mtIndex);

        if (count >= 100)
            return std::nullopt;

        ++count;

        if (const auto& itt = getTableType(index))
        {
            const auto& fit = itt->props.find(name);
            if (fit != itt->props.end())
                return fit->second.type();
        }
        else if (const auto& itf = get<FunctionType>(index))
        {
            std::optional<TypeId> r = first(follow(itf->retTypes));
            if (!r)
                return builtinTypes->nilType;
            else
                return *r;
        }
        else if (get<AnyType>(index))
            return builtinTypes->anyType;
        else
            errors.push_back(TypeError{location, GenericError{"__index should either be a function or table. Got " + toString(index)}});

        mtIndex = findMetatableEntry(builtinTypes, errors, *mtIndex, "__index", location);
    }

    return std::nullopt;
}

std::optional<TypeId> findMetatableEntry(
    NotNull<BuiltinTypes> builtinTypes,
    ErrorVec& errors,
    TypeId type,
    const std::string& entry,
    Location location
)
{
    type = follow(type);

    std::optional<TypeId> metatable = getMetatable(type, builtinTypes);
    if (!metatable)
        return std::nullopt;

    TypeId unwrapped = follow(*metatable);

    if (get<AnyType>(unwrapped))
        return builtinTypes->anyType;

    const TableType* mtt = getTableType(unwrapped);
    if (!mtt)
    {
        errors.push_back(TypeError{location, GenericError{"Metatable was not a table"}});
        return std::nullopt;
    }

    auto it = mtt->props.find(entry);
    if (it != mtt->props.end())
        return it->second.type();
    else
        return std::nullopt;
}

std::optional<TypeId> findTablePropertyRespectingMeta(
    NotNull<BuiltinTypes> builtinTypes,
    ErrorVec& errors,
    TypeId ty,
    const std::string& name,
    Location location
)
{
    return findTablePropertyRespectingMeta(builtinTypes, errors, ty, name, ValueContext::RValue, location);
}

std::optional<TypeId> findTablePropertyRespectingMeta(
    NotNull<BuiltinTypes> builtinTypes,
    ErrorVec& errors,
    TypeId ty,
    const std::string& name,
    ValueContext context,
    Location location
)
{
    if (get<AnyType>(ty))
        return ty;

    if (const TableType* tableType = getTableType(ty))
    {
        const auto& it = tableType->props.find(name);
        if (it != tableType->props.end())
        {
            if (FFlag::LuauSolverV2)
            {
                switch (context)
                {
                case ValueContext::RValue:
                    return it->second.readTy;
                case ValueContext::LValue:
                    return it->second.writeTy;
                }
            }
            else
                return it->second.type();
        }
    }

    std::optional<TypeId> mtIndex = findMetatableEntry(builtinTypes, errors, ty, "__index", location);
    int count = 0;
    while (mtIndex)
    {
        TypeId index = follow(*mtIndex);

        if (count >= 100)
            return std::nullopt;

        ++count;

        if (const auto& itt = getTableType(index))
        {
            const auto& fit = itt->props.find(name);
            if (fit != itt->props.end())
                return fit->second.type();
        }
        else if (const auto& itf = get<FunctionType>(index))
        {
            std::optional<TypeId> r = first(follow(itf->retTypes));
            if (!r)
                return builtinTypes->nilType;
            else
                return *r;
        }
        else if (get<AnyType>(index))
            return builtinTypes->anyType;
        else
            errors.push_back(TypeError{location, GenericError{"__index should either be a function or table. Got " + toString(index)}});

        mtIndex = findMetatableEntry(builtinTypes, errors, *mtIndex, "__index", location);
    }

    return std::nullopt;
}

std::pair<size_t, std::optional<size_t>> getParameterExtents(const TxnLog* log, TypePackId tp, bool includeHiddenVariadics)
{
    size_t minCount = 0;
    size_t optionalCount = 0;

    auto it = begin(tp, log);
    auto endIter = end(tp);

    while (it != endIter)
    {
        TypeId ty = *it;
        if (isOptional(ty))
            ++optionalCount;
        else
        {
            minCount += optionalCount;
            optionalCount = 0;
            minCount++;
        }

        ++it;
    }

    if (it.tail() && isVariadicTail(*it.tail(), *log, includeHiddenVariadics))
        return {minCount, std::nullopt};
    else
        return {minCount, minCount + optionalCount};
}

TypePack extendTypePack(
    TypeArena& arena,
    NotNull<BuiltinTypes> builtinTypes,
    TypePackId pack,
    size_t length,
    std::vector<std::optional<TypeId>> overrides
)
{
    TypePack result;

    while (true)
    {
        pack = follow(pack);

        if (const TypePack* p = get<TypePack>(pack))
        {
            size_t i = 0;
            while (i < p->head.size() && result.head.size() < length)
            {
                result.head.push_back(p->head[i]);
                ++i;
            }

            if (result.head.size() == length)
            {
                if (i == p->head.size())
                    result.tail = p->tail;
                else
                {
                    TypePackId newTail = arena.addTypePack(TypePack{});
                    TypePack* newTailPack = getMutable<TypePack>(newTail);

                    newTailPack->head.insert(newTailPack->head.begin(), p->head.begin() + i, p->head.end());
                    newTailPack->tail = p->tail;

                    result.tail = newTail;
                }

                return result;
            }
            else if (p->tail)
            {
                pack = *p->tail;
                continue;
            }
            else
            {
                // There just aren't enough types in this pack to satisfy the request.
                return result;
            }
        }
        else if (const VariadicTypePack* vtp = get<VariadicTypePack>(pack))
        {
            while (result.head.size() < length)
                result.head.push_back(vtp->ty);
            result.tail = pack;
            return result;
        }
        else if (FreeTypePack* ftp = getMutable<FreeTypePack>(pack))
        {
            // If we need to get concrete types out of a free pack, we choose to
            // interpret this as proof that the pack must have at least 'length'
            // elements.  We mint fresh types for each element we're extracting
            // and rebind the free pack to be a TypePack containing them. We
            // also have to create a new tail.

            TypePack newPack;
            newPack.tail = arena.freshTypePack(ftp->scope);
            if (FFlag::LuauSolverV2)
                result.tail = newPack.tail;
            size_t overridesIndex = 0;
            while (result.head.size() < length)
            {
                TypeId t;
                if (overridesIndex < overrides.size() && overrides[overridesIndex])
                {
                    t = *overrides[overridesIndex];
                }
                else
                {
                    if (FFlag::LuauSolverV2)
                    {
                        FreeType ft{ftp->scope, builtinTypes->neverType, builtinTypes->unknownType};
                        t = arena.addType(ft);
                    }
                    else
                        t = arena.freshType(ftp->scope);
                }

                newPack.head.push_back(t);
                result.head.push_back(newPack.head.back());
                overridesIndex++;
            }

            asMutable(pack)->ty.emplace<TypePack>(std::move(newPack));

            return result;
        }
        else if (auto etp = getMutable<ErrorTypePack>(pack))
        {
            while (result.head.size() < length)
                result.head.push_back(builtinTypes->errorRecoveryType());

            result.tail = pack;
            return result;
        }
        else
        {
            // If the pack is blocked or generic, we can't extract.
            // Return whatever we've got with this pack as the tail.
            result.tail = pack;
            return result;
        }
    }
}

std::vector<TypeId> reduceUnion(const std::vector<TypeId>& types)
{
    std::vector<TypeId> result;
    for (TypeId t : types)
    {
        t = follow(t);
        if (get<NeverType>(t))
            continue;

        if (get<ErrorType>(t) || get<AnyType>(t))
            return {t};

        if (const UnionType* utv = get<UnionType>(t))
        {
            for (TypeId ty : utv)
            {
                ty = follow(ty);
                if (get<NeverType>(ty))
                    continue;
                if (get<ErrorType>(ty) || get<AnyType>(ty))
                    return {ty};

                if (result.end() == std::find(result.begin(), result.end(), ty))
                    result.push_back(ty);
            }
        }
        else if (std::find(result.begin(), result.end(), t) == result.end())
            result.push_back(t);
    }

    return result;
}

static std::optional<TypeId> tryStripUnionFromNil(TypeArena& arena, TypeId ty)
{
    if (const UnionType* utv = get<UnionType>(ty))
    {
        if (!std::any_of(begin(utv), end(utv), isNil))
            return ty;

        std::vector<TypeId> result;

        for (TypeId option : utv)
        {
            if (!isNil(option))
                result.push_back(option);
        }

        if (result.empty())
            return std::nullopt;

        return result.size() == 1 ? result[0] : arena.addType(UnionType{std::move(result)});
    }

    return std::nullopt;
}

TypeId stripNil(NotNull<BuiltinTypes> builtinTypes, TypeArena& arena, TypeId ty)
{
    ty = follow(ty);

    if (get<UnionType>(ty))
    {
        std::optional<TypeId> cleaned = tryStripUnionFromNil(arena, ty);

        // If there is no union option without 'nil'
        if (!cleaned)
            return builtinTypes->nilType;

        return follow(*cleaned);
    }

    return follow(ty);
}

ErrorSuppression shouldSuppressErrors(NotNull<Normalizer> normalizer, TypeId ty)
{
    LUAU_ASSERT(FFlag::LuauSolverV2 || FFlag::LuauAutocompleteRefactorsForIncrementalAutocomplete);
    std::shared_ptr<const NormalizedType> normType = normalizer->normalize(ty);

    if (!normType)
        return ErrorSuppression::NormalizationFailed;

    return (normType->shouldSuppressErrors()) ? ErrorSuppression::Suppress : ErrorSuppression::DoNotSuppress;
}

ErrorSuppression shouldSuppressErrors(NotNull<Normalizer> normalizer, TypePackId tp)
{
    auto [tys, tail] = flatten(tp);

    // check the head, one type at a time
    for (TypeId ty : tys)
    {
        auto result = shouldSuppressErrors(normalizer, ty);
        if (result != ErrorSuppression::DoNotSuppress)
            return result;
    }

    // check the tail if we have one and it's finite
    if (tail && tp != tail && finite(*tail))
        return shouldSuppressErrors(normalizer, *tail);

    return ErrorSuppression::DoNotSuppress;
}

// This is a useful helper because it is often the case that we are looking at specifically a pair of types that might suppress.
ErrorSuppression shouldSuppressErrors(NotNull<Normalizer> normalizer, TypeId ty1, TypeId ty2)
{
    auto result = shouldSuppressErrors(normalizer, ty1);

    // if ty1 is do not suppress, ty2 determines our overall behavior
    if (result == ErrorSuppression::DoNotSuppress)
        return shouldSuppressErrors(normalizer, ty2);

    // otherwise, ty1 is either suppress or normalization failure which are both the appropriate overarching result
    return result;
}

ErrorSuppression shouldSuppressErrors(NotNull<Normalizer> normalizer, TypePackId tp1, TypePackId tp2)
{
    auto result = shouldSuppressErrors(normalizer, tp1);

    // if tp1 is do not suppress, tp2 determines our overall behavior
    if (result == ErrorSuppression::DoNotSuppress)
        return shouldSuppressErrors(normalizer, tp2);

    // otherwise, tp1 is either suppress or normalization failure which are both the appropriate overarching result
    return result;
}

bool isLiteral(const AstExpr* expr)
{
    return (
        expr->is<AstExprTable>() || expr->is<AstExprFunction>() || expr->is<AstExprConstantNumber>() || expr->is<AstExprConstantString>() ||
        expr->is<AstExprConstantBool>() || expr->is<AstExprConstantNil>()
    );
}
/**
 * Visitor which, given an expression and a mapping from expression to TypeId,
 * determines if there are any literal expressions that contain blocked types.
 * This is used for bi-directional inference: we want to "apply" a type from
 * a function argument or a type annotation to a literal.
 */
class BlockedTypeInLiteralVisitor : public AstVisitor
{
public:
    explicit BlockedTypeInLiteralVisitor(NotNull<DenseHashMap<const AstExpr*, TypeId>> astTypes, NotNull<std::vector<TypeId>> toBlock)
        : astTypes_{astTypes}
        , toBlock_{toBlock}
    {
    }
    bool visit(AstNode*) override
    {
        return false;
    }

    bool visit(AstExpr* e) override
    {
        auto ty = astTypes_->find(e);
        if (ty && (get<BlockedType>(follow(*ty)) != nullptr))
        {
            toBlock_->push_back(*ty);
        }
        return isLiteral(e) || e->is<AstExprGroup>();
    }

private:
    NotNull<DenseHashMap<const AstExpr*, TypeId>> astTypes_;
    NotNull<std::vector<TypeId>> toBlock_;
};

std::vector<TypeId> findBlockedTypesIn(AstExprTable* expr, NotNull<DenseHashMap<const AstExpr*, TypeId>> astTypes)
{
    std::vector<TypeId> toBlock;
    BlockedTypeInLiteralVisitor v{astTypes, NotNull{&toBlock}};
    expr->visit(&v);
    return toBlock;
}

std::vector<TypeId> findBlockedArgTypesIn(AstExprCall* expr, NotNull<DenseHashMap<const AstExpr*, TypeId>> astTypes)
{
    std::vector<TypeId> toBlock;
    BlockedTypeInLiteralVisitor v{astTypes, NotNull{&toBlock}};
    for (auto arg: expr->args)
    {
        if (isLiteral(arg) || arg->is<AstExprGroup>())
        {
            arg->visit(&v);
        }
    }
    return toBlock;
}


} // namespace Luau
