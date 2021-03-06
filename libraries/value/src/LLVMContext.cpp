////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     LLVMContext.cpp (value)
//  Authors:  Kern Handa
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "LLVMContext.h"
#include "FunctionDeclaration.h"
#include "Scalar.h"
#include "Value.h"

#include <emitters/include/IRModuleEmitter.h>

#include <utilities/include/StringUtil.h>

#include <llvm/Support/raw_os_ostream.h>

using namespace std::string_literals;

namespace ell
{
namespace value
{
    using namespace detail;
    using namespace emitters;
    using namespace utilities;

    namespace
    {

        detail::ValueTypeDescription LLVMTypeToValueType(LLVMType type)
        {
            switch (type->getTypeID())
            {
            case llvm::Type::TypeID::FloatTyID:
                return { ValueType::Float, 0 };
            case llvm::Type::TypeID::DoubleTyID:
                return { ValueType::Double, 0 };
            case llvm::Type::TypeID::IntegerTyID:
                switch (type->getIntegerBitWidth())
                {
                case 1:
                    return { ValueType::Boolean, 0 };
                case 8:
                    return { ValueType::Char8, 0 };
                case 16:
                    return { ValueType::Int16, 0 };
                case 32:
                    return { ValueType::Int32, 0 };
                case 64:
                    return { ValueType::Int64, 0 };
                default:
                    break;
                }
            case llvm::Type::TypeID::PointerTyID:
            {
                auto elementType = type->getPointerElementType();

                auto underlyingType = LLVMTypeToValueType(elementType);
                underlyingType.second += 1;
                return underlyingType;
            }
            case llvm::Type::TypeID::ArrayTyID:
            {
                auto elementType = type->getArrayElementType();

                auto underlyingType = LLVMTypeToValueType(elementType);
                underlyingType.second += 1;
                return underlyingType;
            }
            default:
                break;
            }
            throw LogicException(LogicExceptionErrors::illegalState);
        }

        LLVMType ValueTypeToLLVMType(IREmitter& emitter, detail::ValueTypeDescription typeDescription)
        {
            auto& builder = emitter.GetIRBuilder();
            LLVMType type = nullptr;
            switch (typeDescription.first)
            {
            case ValueType::Boolean:
                if (typeDescription.second == 0)
                {
                    type = builder.getInt1Ty();
                }
                else
                {
                    type = builder.getInt8Ty();
                }
                break;
            case ValueType::Byte:
                [[fallthrough]];
            case ValueType::Char8:
                type = builder.getInt8Ty();
                break;
            case ValueType::Int16:
                type = builder.getInt16Ty();
                break;
            case ValueType::Int32:
                type = builder.getInt32Ty();
                break;
            case ValueType::Int64:
                type = builder.getInt64Ty();
                break;
            case ValueType::Float:
                type = builder.getFloatTy();
                break;
            case ValueType::Double:
                type = builder.getDoubleTy();
                break;
            case ValueType::Void:
                type = builder.getVoidTy();
                break;
            case ValueType::Undefined:
                [[fallthrough]];
            default:
                throw LogicException(LogicExceptionErrors::illegalState);
            }

            for (int ptrLevel = 0; ptrLevel < typeDescription.second; ++ptrLevel)
            {
                type = type->getPointerTo();
            }

            return type;
        }

        VariableType ValueTypeToVariableType(ValueType type)
        {
            // clang-format off

#define VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(x, y)  \
    case ValueType::x:                             \
        return VariableType::y

#define VALUE_TYPE_TO_VARIABLE_TYPE_PTR(x)         \
    case ValueType::x:                             \
        return VariableType::x##Pointer

            // clang-format on

            switch (type)
            {
                VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(Boolean, Boolean);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Byte);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Char8);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int16);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int32);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int64);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Float);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Double);
                VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(Void, Void);
            default:
                throw LogicException(LogicExceptionErrors::illegalState);
            }

#undef VALUE_TYPE_TO_VARIABLE_TYPE_PTR

#undef VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING
        }

        // TODO: Make this the basis of an iterator for MemoryLayout
        bool IncrementMemoryCoordinateImpl(int dimension, std::vector<int>& coordinate, const std::vector<int>& maxCoordinate)
        {
            // base case
            if (dimension < 0)
            {
                return false;
            }

            if (++coordinate[dimension] >= maxCoordinate[dimension])
            {
                coordinate[dimension] = 0;
                return IncrementMemoryCoordinateImpl(dimension - 1, coordinate, maxCoordinate);
            }

            return true;
        }

        bool IncrementMemoryCoordinate(std::vector<int>& coordinate, const std::vector<int>& maxCoordinate)
        {
            assert(coordinate.size() == maxCoordinate.size());
            return IncrementMemoryCoordinateImpl(static_cast<int>(maxCoordinate.size()) - 1, coordinate, maxCoordinate);
        }

        LLVMValue ToLLVMValue(Value value) { return value.Get<Emittable>().GetDataAs<LLVMValue>(); }

        auto SimpleNumericalFunctionIntrinsic(IRFunctionEmitter& fnEmitter, LLVMFunction (IRRuntime::*intrinsicFn)(VariableType)) -> std::function<Value(std::vector<Value>)>
        {
            return [&fnEmitter, intrinsicFn](std::vector<Value> args) -> Value {
                if (args.size() != 1)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                const auto& value = args[0];
                if (value.GetBaseType() == ValueType::Boolean)
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                auto variableType = [](ValueType type) {
                    switch (type)
                    {
                    case ValueType::Float:
                        return VariableType::Float;
                    default:
                        return VariableType::Double;
                    }
                }(value.GetBaseType());

                auto llvmFunc = std::invoke(intrinsicFn, fnEmitter.GetModule().GetRuntime(), variableType);

                Value returnValue = value::Allocate(value.GetBaseType(),
                                                    value.IsConstrained() ? value.GetLayout() : ScalarLayout);

                const auto& returnLayout = returnValue.GetLayout();
                auto maxCoordinate = returnLayout.GetActiveSize().ToVector();
                decltype(maxCoordinate) coordinate(maxCoordinate.size());
                auto inputLLVMValue = ToLLVMValue(value);
                auto returnLLVMValue = ToLLVMValue(returnValue);
                do
                {
                    auto logicalCoordinates = returnLayout.GetLogicalCoordinates(coordinate);
                    auto offset = static_cast<int>(returnLayout.GetLogicalEntryOffset(logicalCoordinates));

                    LLVMValue resultValue = nullptr;
                    if (value.IsFloatingPoint() || value.IsFloatingPointPointer())
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.ValueAt(inputLLVMValue, offset) });
                    }
                    else
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.CastValue<double>(fnEmitter.ValueAt(inputLLVMValue, offset)) });
                    }
                    fnEmitter.SetValueAt(returnLLVMValue, offset, resultValue);
                } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                return returnValue;
            };
        }

        auto PowFunctionIntrinsic(IRFunctionEmitter& fnEmitter) -> std::function<Value(std::vector<Value>)>
        {
            return [&fnEmitter](std::vector<Value> args) -> Value {
                if (args.size() != 2)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                const auto& value1 = args[0];
                const auto& value2 = args[1];
                if (value1.GetBaseType() != value2.GetBaseType())
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if (value1.GetBaseType() == ValueType::Boolean)
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if (value2.IsConstrained() && value2.GetLayout() != ScalarLayout)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                auto variableType = [type = value1.GetBaseType()] {
                    switch (type)
                    {
                    case ValueType::Float:
                        return VariableType::Float;
                    default:
                        return VariableType::Double;
                    }
                }();

                auto llvmFunc = fnEmitter.GetModule().GetRuntime().GetPowFunction(variableType);

                Value returnValue = value::Allocate(value1.GetBaseType(),
                                                    value1.IsConstrained() ? value1.GetLayout() : ScalarLayout);

                const auto& returnLayout = returnValue.GetLayout();
                auto maxCoordinate = returnLayout.GetActiveSize().ToVector();
                decltype(maxCoordinate) coordinate(maxCoordinate.size());
                auto expLLVMValue = [&] {
                    if (value2.IsFloatingPoint() || value2.IsFloatingPointPointer())
                    {
                        return fnEmitter.ValueAt(ToLLVMValue(value2), 0);
                    }
                    else
                    {
                        return fnEmitter.CastValue<double>(fnEmitter.ValueAt(ToLLVMValue(value2), 0));
                    }
                }();
                auto baseLLVMValue = ToLLVMValue(value1);
                auto returnLLVMValue = ToLLVMValue(returnValue);
                do
                {
                    auto logicalCoordinates = returnLayout.GetLogicalCoordinates(coordinate);
                    auto offset = static_cast<int>(returnLayout.GetLogicalEntryOffset(logicalCoordinates));

                    LLVMValue resultValue = nullptr;
                    if (value1.IsFloatingPoint() || value1.IsFloatingPointPointer())
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.ValueAt(baseLLVMValue, offset), expLLVMValue });
                    }
                    else
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.CastValue<double>(fnEmitter.ValueAt(baseLLVMValue, offset)), expLLVMValue });
                    }
                    fnEmitter.SetValueAt(returnLLVMValue, offset, resultValue);

                } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                return returnValue;
            };
        }

        enum class MaxMinIntrinsic
        {
            Max,
            Min
        };

        auto MaxMinIntrinsicFunction(IRFunctionEmitter& fnEmitter, MaxMinIntrinsic intrinsic) -> std::function<Value(std::vector<Value>)>
        {
            return [&fnEmitter, intrinsic](std::vector<Value> args) -> Value {
                if (args.size() == 1)
                {
                    const auto& value = args[0];
                    if (value.GetBaseType() == ValueType::Boolean)
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    Value result = value::Allocate(value.GetBaseType(), ScalarLayout);
                    auto cmpOp = [&] {
                        switch (value.GetBaseType())
                        {
                        case ValueType::Float:
                            [[fallthrough]];
                        case ValueType::Double:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEqualsFloat;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEqualsFloat;
                            }
                        default:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEquals;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEquals;
                            }
                        }
                    }();
                    auto inputLLVMValue = ToLLVMValue(value);
                    auto resultLLVMValue = ToLLVMValue(result);

                    // set the initial value
                    fnEmitter.SetValueAt(resultLLVMValue, 0, fnEmitter.ValueAt(inputLLVMValue, 0));

                    const auto& inputLayout = value.GetLayout();
                    auto maxCoordinate = inputLayout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());

                    do
                    {
                        auto logicalCoordinates = inputLayout.GetLogicalCoordinates(coordinate);
                        auto offset = static_cast<int>(inputLayout.GetLogicalEntryOffset(logicalCoordinates));

                        auto op1 = fnEmitter.ValueAt(resultLLVMValue, 0);
                        auto op2 = fnEmitter.ValueAt(inputLLVMValue, offset);
                        auto cmp = fnEmitter.Comparison(cmpOp, op1, op2);
                        auto selected = fnEmitter.Select(cmp, op1, op2);
                        fnEmitter.SetValueAt(resultLLVMValue, 0, selected);
                    } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                    return result;
                }
                else if (args.size() == 2)
                {
                    const auto& value1 = args[0];
                    const auto& value2 = args[1];
                    if (value1.GetBaseType() != value2.GetBaseType())
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    if (value1.GetBaseType() == ValueType::Boolean)
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    if ((value1.IsConstrained() && value1.GetLayout() != ScalarLayout) ||
                        (value2.IsConstrained() && value2.GetLayout() != ScalarLayout))
                    {
                        throw InputException(InputExceptionErrors::invalidSize);
                    }

                    Value result = value::Allocate(value1.GetBaseType(), ScalarLayout);
                    auto cmpOp = [&] {
                        switch (value1.GetBaseType())
                        {
                        case ValueType::Float:
                            [[fallthrough]];
                        case ValueType::Double:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEqualsFloat;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEqualsFloat;
                            }
                        default:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEquals;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEquals;
                            }
                        }
                    }();
                    auto llvmValue1 = fnEmitter.ValueAt(ToLLVMValue(value1), 0);
                    auto llvmValue2 = fnEmitter.ValueAt(ToLLVMValue(value2), 0);
                    auto cmp = fnEmitter.Comparison(cmpOp, llvmValue1, llvmValue2);

                    auto resultValue = ToLLVMValue(result);
                    fnEmitter.SetValueAt(resultValue, 0, fnEmitter.Select(cmp, llvmValue1, llvmValue2));
                    return result;
                }
                else
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }
            };
        }

    } // namespace

    struct LLVMContext::FunctionScope
    {
        template <typename... Args>
        FunctionScope(LLVMContext& context, Args&&... args) :
            context(context)
        {
            context._functionStack.push(context._emitter.BeginFunction(std::forward<Args>(args)...));
            context._promotedConstantStack.push({});
        }

        ~FunctionScope()
        {
            context._functionStack.pop();
            context._promotedConstantStack.pop();
        }

        LLVMContext& context;
    };

    LLVMContext::LLVMContext(IRModuleEmitter& emitter) :
        _emitter(emitter),
        _computeContext(_emitter.GetModuleName())
    {
        _promotedConstantStack.push({});
    }

    const IRModuleEmitter& LLVMContext::GetModuleEmitter() const { return _emitter; }

    Value LLVMContext::AllocateImpl(ValueType type, MemoryLayout layout)
    {
        auto llvmType = ValueTypeToLLVMType(GetFunctionEmitter().GetEmitter(), { type, 0 });
        auto allocatedVariable = GetFunctionEmitter().Variable(llvmType, layout.GetMemorySize());

        auto& fn = GetFunctionEmitter();
        auto& irEmitter = fn.GetEmitter();
        irEmitter.MemorySet(allocatedVariable, irEmitter.Zero(llvm::Type::getInt8Ty(fn.GetLLVMContext())), irEmitter.Literal(static_cast<int64_t>(layout.GetMemorySize() * irEmitter.SizeOf(llvmType))));
        return { Emittable{ allocatedVariable }, layout };
    }

    std::optional<Value> LLVMContext::GetGlobalValue(GlobalAllocationScope scope, std::string name)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);
        if (auto it = _globals.find(adjustedName); it != _globals.end())
        {
            return Value(it->second.first, it->second.second);
        }

        return std::nullopt;
    }

    Value LLVMContext::GlobalAllocateImpl(GlobalAllocationScope scope, std::string name, ConstantData data, MemoryLayout layout)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);

        if (_globals.find(adjustedName) != _globals.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument,
                                 "Unexpected collision in global data allocation");
        }

        llvm::GlobalVariable* global = std::visit(
            [this, &adjustedName](auto&& vectorData) {
                using Type = std::decay_t<decltype(vectorData)>;

                if constexpr (std::is_same_v<Type, std::vector<utilities::Boolean>>)
                {
                    // IREmitter stores a vector of bool values as a bitvector, which
                    // breaks the memory model we need for our purposes.
                    // NB: This somewhat screws up our type system because we rely
                    // on LLVM to tell us the type, but here we set a different type
                    // altogether, with no discernable way of retrieving the fact that
                    // originally, this was a vector of bools. This will be rectified
                    // in the near future. (2018-11-08)
                    std::vector<char> transformedData(vectorData.begin(), vectorData.end());
                    return _emitter.GlobalArray(adjustedName, transformedData);
                }
                else
                {
                    return _emitter.GlobalArray(adjustedName, vectorData);
                }
            },
            data);
        auto dereferencedGlobal = _emitter.GetIREmitter().PointerOffset(global, _emitter.GetIREmitter().Literal(0));

        Emittable emittable{ dereferencedGlobal };
        _globals[adjustedName] = { emittable, layout };

        return Value(emittable, layout);
    }

    Value LLVMContext::GlobalAllocateImpl(GlobalAllocationScope scope, std::string name, ValueType type, MemoryLayout layout)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);

        if (_globals.find(adjustedName) != _globals.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument,
                                 FormatString("Global variable %s is already defined", adjustedName.c_str()));
        }

        auto global = _emitter.GlobalArray(adjustedName,
                                           ValueTypeToLLVMType(_emitter.GetIREmitter(), { type, 0 }),
                                           layout.GetMemorySize());

        auto dereferencedGlobal = _emitter.GetIREmitter().PointerOffset(global, _emitter.GetIREmitter().Literal(0));

        Emittable emittable{ dereferencedGlobal };
        _globals[adjustedName] = { emittable, layout };

        return Value(emittable, layout);
    }

    detail::ValueTypeDescription LLVMContext::GetTypeImpl(Emittable emittable)
    {
        auto value = emittable.GetDataAs<LLVMValue>();
        auto type = value->getType();
        return LLVMTypeToValueType(type);
    }

    EmitterContext::DefinedFunction LLVMContext::CreateFunctionImpl(FunctionDeclaration decl, EmitterContext::DefinedFunction fn)
    {
        if (const auto& intrinsics = GetIntrinsics();
            std::find(intrinsics.begin(), intrinsics.end(), decl) != intrinsics.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument, "Specified function is an intrinsic");
        }

        if (auto it = _definedFunctions.find(decl); it != _definedFunctions.end())
        {
            return it->second;
        }

        const auto& argValues = decl.GetParameterTypes();
        const auto& returnValue = decl.GetReturnType();

        std::vector<VariableType> variableArgTypes(argValues.size());
        std::transform(argValues.begin(), argValues.end(), variableArgTypes.begin(), [](Value value) {
            return ValueTypeToVariableType(value.GetBaseType());
        });

        const auto& fnName = decl.GetFunctionName();
        {
            ValueType returnValueType = returnValue ? returnValue->GetBaseType() : ValueType::Void;
            FunctionScope scope(*this, fnName, ValueTypeToVariableType(returnValueType), variableArgTypes);
            GetFunctionEmitter().SetAttributeForArguments(IRFunctionEmitter::Attributes::NoAlias);

            auto functionArgs = GetFunctionEmitter().Arguments();
            auto argValuesCopy = argValues;
            auto returnValueCopy = returnValue;

            for (std::pair idx{ 0u, functionArgs.begin() }; idx.first < argValuesCopy.size(); ++idx.first, ++idx.second)
            {
                idx.second->setName(std::string{ "arg" } + std::to_string(idx.first));
                argValuesCopy[idx.first].SetData(Emittable{ idx.second });
            }

            returnValueCopy = fn(argValuesCopy);
            if (returnValueCopy)
            {
                _emitter.EndFunction(EnsureEmittable(*returnValueCopy).Get<Emittable>().GetDataAs<LLVMValue>());
            }
            else
            {
                _emitter.EndFunction();
            }
        }

        DefinedFunction returnFn = [this, decl](std::vector<Value> args) -> std::optional<Value> {
            const auto& argValues = decl.GetParameterTypes();
            const auto& returnValue = decl.GetReturnType();
            const auto& fnName = decl.GetFunctionName();

            if (!std::equal(args.begin(),
                            args.end(),
                            argValues.begin(),
                            argValues.end(),
                            [](Value suppliedValue, Value fnValue) {
                                return suppliedValue.GetBaseType() == fnValue.GetBaseType();
                            }))
            {
                throw InputException(InputExceptionErrors::invalidArgument);
            }

            std::vector<LLVMValue> llvmArgs(args.size());
            std::transform(args.begin(), args.end(), llvmArgs.begin(), [this](Value& arg) {
                return EnsureEmittable(arg).Get<Emittable>().GetDataAs<LLVMValue>();
            });

            auto returnValueCopy = returnValue;
            LLVMValue fnReturnValue = _emitter.GetCurrentFunction().Call(fnName, llvmArgs);
            if (returnValueCopy)
            {
                returnValueCopy->SetData(Emittable{ fnReturnValue });
            }
            return returnValueCopy;
        };

        _definedFunctions[decl] = returnFn;

        return returnFn;
    }

    bool LLVMContext::IsFunctionDefinedImpl(FunctionDeclaration decl) const
    {
        if (const auto& intrinsics = GetIntrinsics();
            std::find(intrinsics.begin(), intrinsics.end(), decl) != intrinsics.end())
        {
            return true;
        }

        return _definedFunctions.find(decl) != _definedFunctions.end();
    }

    Value LLVMContext::StoreConstantDataImpl(ConstantData data) { return _computeContext.StoreConstantData(data); }

    void LLVMContext::ForImpl(MemoryLayout layout, std::function<void(std::vector<Scalar>)> fn)
    {
        auto maxCoordinate = layout.GetActiveSize().ToVector();
        decltype(maxCoordinate) coordinate(maxCoordinate.size());

        do
        {
            auto logicalCoordinates = layout.GetLogicalCoordinates(coordinate).ToVector();
            fn(std::vector<Scalar>(logicalCoordinates.begin(), logicalCoordinates.end()));
        } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));
    }

    void LLVMContext::MoveDataImpl(Value& source, Value& destination)
    {
        // we treat a move the same as a copy, except we clear out the source
        CopyDataImpl(source, destination);

        // data has been "moved", so clear the source
        source.Reset();
    }

    void LLVMContext::CopyDataImpl(const Value& source, Value& destination)
    {
        if (destination.IsConstant())
        {
            if (source.IsConstant())
            {
                return _computeContext.CopyData(source, destination);
            }
            else
            {
                throw LogicException(LogicExceptionErrors::illegalState);
            }
        }
        else
        {
            if (!TypeCompatible(destination, source) &&
                (destination.PointerLevel() == source.PointerLevel() ||
                 destination.PointerLevel() == (1 + source.PointerLevel())))
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }

            auto& irEmitter = _emitter.GetIREmitter();
            auto destValue = ToLLVMValue(destination);
            if (source.IsConstant())
            {
                // we're only copying active areas below. should we copy padded too?
                auto& layout = source.GetLayout();
                std::visit(
                    VariantVisitor{ [](Undefined) {},
                                    [](Emittable) {},
                                    [destValue, &irEmitter, &layout](auto&& data) {
                                        auto maxCoordinate = layout.GetActiveSize().ToVector();
                                        decltype(maxCoordinate) coordinate(maxCoordinate.size());

                                        do
                                        {
                                            auto srcAtOffset =
                                                irEmitter.Literal(*(data + layout.GetEntryOffset(coordinate)));
                                            auto destOffset = irEmitter.Literal(
                                                static_cast<int>(layout.GetEntryOffset(coordinate)));
                                            auto destAtOffset = irEmitter.PointerOffset(destValue, destOffset);
                                            (void)irEmitter.Store(destAtOffset, srcAtOffset);
                                        } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));
                                    } },
                    source.GetUnderlyingData());
            }
            else
            {
                auto srcValue = ToLLVMValue(source);
                if (srcValue == destValue)
                {
                    return;
                }
                if (auto& layout = source.GetLayout(); layout.IsContiguous())
                {
                    if (destination.PointerLevel() == source.PointerLevel())
                    {
                        irEmitter.MemoryCopy(srcValue,
                                             destValue,
                                             irEmitter.Literal(
                                                 static_cast<int64_t>(
                                                     layout.GetMemorySize() * irEmitter.SizeOf(srcValue->getType()))));
                    }
                    else
                    {
                        auto destAtOffset = irEmitter.PointerOffset(destValue, irEmitter.Zero(VariableType::Int32));
                        irEmitter.Store(destAtOffset, srcValue);
                    }
                }
                else
                {
                    auto maxCoordinate = layout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());

                    do
                    {
                        auto offset = irEmitter.Literal(static_cast<int>(layout.GetEntryOffset(coordinate)));
                        auto srcAtOffset = irEmitter.PointerOffset(srcValue, offset);
                        auto destAtOffset = irEmitter.PointerOffset(destValue, offset);

                        [[maybe_unused]] auto storeInst = irEmitter.Store(destAtOffset, irEmitter.Load(srcAtOffset));

                    } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));
                }
            }
        }
    }

    Value LLVMContext::OffsetImpl(Value begin, Value index)
    {
        if (begin.IsConstant() && index.IsConstant())
        {
            return _computeContext.Offset(begin, index);
        }
        else
        {
            auto& fn = GetFunctionEmitter();
            Value emittableBegin = EnsureEmittable(begin);
            Value emittableIndex = EnsureEmittable(index);

            auto llvmBegin = std::get<Emittable>(emittableBegin.GetUnderlyingData()).GetDataAs<LLVMValue>();
            auto llvmIndex = std::get<Emittable>(emittableIndex.GetUnderlyingData()).GetDataAs<LLVMValue>();

            return Emittable{ fn.PointerOffset(llvmBegin, fn.ValueAt(llvmIndex, 0)) };
        }
    }

    Value LLVMContext::UnaryOperationImpl(ValueUnaryOperation op, Value destination)
    {
        throw LogicException(LogicExceptionErrors::notImplemented);
    }

    Value LLVMContext::BinaryOperationImpl(ValueBinaryOperation op, Value destination, Value source)
    {
        if (!source.IsDefined())
        {
            throw InputException(InputExceptionErrors::invalidArgument);
        }

        if (destination.IsDefined())
        {
            if (source.IsConstant() && destination.IsConstant())
            {
                return _computeContext.BinaryOperation(op, destination, source);
            }
        }
        else
        {
            destination = Allocate(source.GetBaseType(), source.GetLayout());
        }

        if (!TypeCompatible(destination, source))
        {
            throw InputException(InputExceptionErrors::typeMismatch);
        }

        if (destination.GetLayout() != source.GetLayout())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        auto& fn = GetFunctionEmitter();
        std::visit(
            [destination = EnsureEmittable(destination), op, &fn](auto&& sourceData) {
                using SourceDataType = std::decay_t<decltype(sourceData)>;
                if constexpr (std::is_same_v<SourceDataType, Undefined>)
                {
                }
                else if constexpr (std::is_same_v<Boolean*, SourceDataType>)
                {
                    throw LogicException(LogicExceptionErrors::notImplemented);
                }
                else
                {
                    auto isFp = destination.IsFloatingPoint();
                    std::function<LLVMValue(LLVMValue, LLVMValue)> opFn;
                    switch (op)
                    {
                    case ValueBinaryOperation::add:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::addFloat : TypedOperator::add, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::subtract:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::subtractFloat : TypedOperator::subtract, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::multiply:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::multiplyFloat : TypedOperator::multiply, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::divide:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::divideFloat : TypedOperator::divideSigned,
                                               dst,
                                               src);
                        };
                        break;
                    case ValueBinaryOperation::modulus:
                        if (isFp)
                        {
                            throw InputException(InputExceptionErrors::invalidArgument);
                        }
                        opFn = [&fn](auto dst, auto src) { return fn.Operator(TypedOperator::moduloSigned, dst, src); };
                        break;
                    default:
                        throw LogicException(LogicExceptionErrors::illegalState);
                    }

                    auto& layout = destination.GetLayout();
                    auto maxCoordinate = layout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());

                    auto destValue = ToLLVMValue(destination);

                    std::conditional_t<std::is_same_v<Emittable, SourceDataType>, LLVMValue, SourceDataType> srcValue =
                        nullptr;
                    if constexpr (std::is_same_v<Emittable, SourceDataType>)
                    {
                        srcValue = sourceData.template GetDataAs<LLVMValue>();
                    }
                    else
                    {
                        srcValue = sourceData;
                    }

                    do
                    {
                        LLVMValue opResult = nullptr;
                        if constexpr (std::is_same_v<Emittable, SourceDataType>)
                        {
                            opResult =
                                opFn(fn.ValueAt(destValue,
                                                fn.Literal(static_cast<int>(layout.GetEntryOffset(coordinate)))),
                                     fn.ValueAt(srcValue,
                                                fn.Literal(static_cast<int>(layout.GetEntryOffset(coordinate)))));
                        }
                        else
                        {
                            opResult = opFn(fn.ValueAt(destValue,
                                                       fn.Literal(static_cast<int>(layout.GetEntryOffset(coordinate)))),
                                            fn.Literal(*(srcValue + layout.GetEntryOffset(coordinate))));
                        }

                        fn.SetValueAt(destValue,
                                      fn.Literal(static_cast<int>(layout.GetEntryOffset(coordinate))),
                                      opResult);
                    } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));
                }
            },
            source.GetUnderlyingData());

        return destination;
    }

    Value LLVMContext::LogicalOperationImpl(ValueLogicalOperation op, Value source1, Value source2)
    {
        if (source1.GetLayout() != source2.GetLayout())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        if (source1.IsConstant() && source2.IsConstant())
        {
            return _computeContext.LogicalOperation(op, source1, source2);
        }

        auto comparisonOp = TypedComparison::none;
        bool isFp = source1.IsFloatingPoint() || source1.IsFloatingPointPointer();
        switch (op)
        {
        case ValueLogicalOperation::equality:
            comparisonOp = isFp ? TypedComparison::equalsFloat : TypedComparison::equals;
            break;
        case ValueLogicalOperation::inequality:
            comparisonOp = isFp ? TypedComparison::notEqualsFloat : TypedComparison::notEquals;
            break;
        case ValueLogicalOperation::greaterthan:
            comparisonOp = isFp ? TypedComparison::greaterThanFloat : TypedComparison::greaterThan;
            break;
        case ValueLogicalOperation::greaterthanorequal:
            comparisonOp = isFp ? TypedComparison::greaterThanOrEqualsFloat : TypedComparison::greaterThanOrEquals;
            break;
        case ValueLogicalOperation::lessthan:
            comparisonOp = isFp ? TypedComparison::lessThanFloat : TypedComparison::lessThan;
            break;
        case ValueLogicalOperation::lessthanorequal:
            comparisonOp = isFp ? TypedComparison::lessThanOrEqualsFloat : TypedComparison::lessThanOrEquals;
            break;
        }

        Value returnValue = std::visit(
            VariantVisitor{
                [](Undefined) -> Value { throw LogicException(LogicExceptionErrors::illegalState); },
                [this,
                 comparisonOp,
                 &source2UnderlyingData = source2.GetUnderlyingData(),
                 &source1Layout = source1.GetLayout(),
                 &source2Layout = source2.GetLayout()](Emittable source1Data) -> Value {
                    // source1 is an Emittable type, so source2 can be constant or Emittable
                    return std::
                        visit(VariantVisitor{ [](Undefined) -> Value {
                                                 throw LogicException(LogicExceptionErrors::illegalState);
                                             },
                                              [&, this](Emittable source2Data) -> Value {
                                                  auto maxCoordinate = source1Layout.GetActiveSize().ToVector();
                                                  decltype(maxCoordinate) coordinate(maxCoordinate.size());
                                                  auto& fn = this->GetFunctionEmitter();
                                                  auto result = fn.TrueBit();
                                                  auto llvmOp1 = source1Data.GetDataAs<LLVMValue>();
                                                  auto llvmOp2 = source2Data.GetDataAs<LLVMValue>();
                                                  do
                                                  {
                                                      auto logicalCoordinates =
                                                          source1Layout.GetLogicalCoordinates(coordinate);
                                                      auto source1Offset =
                                                          source1Layout.GetLogicalEntryOffset(logicalCoordinates);
                                                      auto source2Offset =
                                                          source2Layout.GetLogicalEntryOffset(logicalCoordinates);

                                                      result = fn.LogicalAnd(result,
                                                                             fn.Comparison(comparisonOp,
                                                                                           fn.ValueAt(llvmOp1,
                                                                                                      source1Offset),
                                                                                           fn.ValueAt(llvmOp2,
                                                                                                      source2Offset)));

                                                  } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                                                  return { Emittable{ result }, ScalarLayout };
                                              },
                                              [&, this](auto&& source2Data) -> Value {
                                                  using Type =
                                                      std::remove_pointer_t<std::decay_t<decltype(source2Data)>>;
                                                  using CastType =
                                                      std::conditional_t<std::is_same_v<Type, Boolean>, bool, Type>;
                                                  auto& fn = this->GetFunctionEmitter();

                                                  auto result = fn.TrueBit();
                                                  auto llvmOp1 = source1Data.GetDataAs<LLVMValue>();
                                                  auto maxCoordinate = source1Layout.GetActiveSize().ToVector();
                                                  decltype(maxCoordinate) coordinate(maxCoordinate.size());
                                                  do
                                                  {
                                                      auto logicalCoordinates =
                                                          source1Layout.GetLogicalCoordinates(coordinate);
                                                      auto source1Offset =
                                                          source1Layout.GetLogicalEntryOffset(logicalCoordinates);
                                                      auto source2Offset =
                                                          source2Layout.GetLogicalEntryOffset(logicalCoordinates);

                                                      result =
                                                          fn.LogicalAnd(result,
                                                                        fn.Comparison(comparisonOp,
                                                                                      fn.ValueAt(llvmOp1,
                                                                                                 source1Offset),
                                                                                      fn.Literal(static_cast<CastType>(
                                                                                          source2Data
                                                                                              [source2Offset]))));

                                                  } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                                                  return { Emittable{ result }, ScalarLayout };
                                              } },
                              source2UnderlyingData);
                },
                [this,
                 comparisonOp,
                 &source2Data = source2.GetUnderlyingData(),
                 &source1Layout = source1.GetLayout(),
                 &source2Layout = source2.GetLayout()](auto&& source1Data) -> Value {
                    // source1 is constant, so source2 has to be an Emittable type
                    using Type = std::remove_pointer_t<std::decay_t<decltype(source1Data)>>;
                    using CastType = std::conditional_t<std::is_same_v<Type, Boolean>, bool, Type>;

                    auto& fn = GetFunctionEmitter();
                    auto result = fn.TrueBit();
                    auto llvmOp2 = std::get<Emittable>(source2Data).GetDataAs<LLVMValue>();

                    auto maxCoordinate = source1Layout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());
                    do
                    {
                        auto logicalCoordinates = source1Layout.GetLogicalCoordinates(coordinate);
                        auto source1Offset = source1Layout.GetLogicalEntryOffset(logicalCoordinates);
                        auto source2Offset = source2Layout.GetLogicalEntryOffset(logicalCoordinates);

                        result =
                            fn.LogicalAnd(result,
                                          fn.Comparison(comparisonOp,
                                                        fn.Literal(static_cast<CastType>(source1Data[source1Offset])),
                                                        fn.ValueAt(llvmOp2, source2Offset)));

                    } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                    return { Emittable{ result }, ScalarLayout };
                } },
            source1.GetUnderlyingData());

        return returnValue;
    }

    Value LLVMContext::CastImpl(Value value, ValueType type)
    {
        if (value.IsConstant())
        {
            return _computeContext.Cast(value, type);
        }

        auto data = ToLLVMValue(value);
        auto& fn = GetFunctionEmitter();

        auto castedData = Allocate(type, value.IsConstrained() ? value.GetLayout() : ScalarLayout);
        auto castedValue = ToLLVMValue(castedData);
        for (size_t index = 0u; index < castedData.GetLayout().GetMemorySize(); ++index)
        {
            fn.SetValueAt(
                castedValue,
                static_cast<int>(index),
                fn.CastValue(
                    fn.ValueAt(data, static_cast<int>(index)),
                    ValueTypeToLLVMType(fn.GetEmitter(), { type, 0 })));
        }

        return castedData;
    }

    class LLVMContext::IfContextImpl : public EmitterContext::IfContextImpl
    {
    public:
        IfContextImpl(IRIfEmitter ifEmitter, IRFunctionEmitter& fnEmitter) :
            _ifEmitter(std::move(ifEmitter)),
            _fnEmitter(fnEmitter)
        {}

        void ElseIf(Scalar test, std::function<void()> fn) override
        {
            LLVMValue testValue = nullptr;
            if (auto value = test.GetValue(); value.IsConstant())
            {
                testValue = _fnEmitter.Literal(static_cast<bool>(test.Get<Boolean>()));
            }
            else
            {
                testValue = ToLLVMValue(value);
            }

            _ifEmitter.ElseIf(testValue, [fn = std::move(fn)](auto&) { fn(); });
        }

        void Else(std::function<void()> fn) override
        {
            _ifEmitter.Else([fn = std::move(fn)](auto&) { fn(); });
        }

    private:
        IRIfEmitter _ifEmitter;
        IRFunctionEmitter& _fnEmitter;
    };

    EmitterContext::IfContext LLVMContext::IfImpl(Scalar test, std::function<void()> fn)
    {
        auto& fnEmitter = GetFunctionEmitter();
        LLVMValue testValue = nullptr;
        if (auto value = test.GetValue(); value.IsConstant())
        {
            testValue = fnEmitter.Literal(static_cast<bool>(test.Get<Boolean>()));
        }
        else
        {
            testValue = ToLLVMValue(value);
        }

        auto ifEmitter = fnEmitter.If(testValue, [fn = std::move(fn)](auto&) { fn(); });

        return { std::make_unique<LLVMContext::IfContextImpl>(std::move(ifEmitter), fnEmitter) };
    }

    std::optional<Value> LLVMContext::CallImpl(FunctionDeclaration func, std::vector<Value> args)
    {
        if (std::any_of(args.begin(), args.end(), [](const auto& value) { return value.IsEmpty(); }))
        {
            throw InputException(InputExceptionErrors::invalidArgument);
        }

        const auto& intrinsics = GetIntrinsics();
        if (std::find(intrinsics.begin(), intrinsics.end(), func) != intrinsics.end())
        {
            return IntrinsicCall(func, args);
        }

        if (auto it = _definedFunctions.find(func); it != _definedFunctions.end())
        {
            return it->second(args);
        }

        return EmitExternalCall(func, args);
    }

    void LLVMContext::DebugDumpImpl(Value value, std::string tag, std::ostream& stream) const
    {
        auto realizedValue = Realize(value);
        if (realizedValue.IsConstant())
        {
            _computeContext.DebugDump(realizedValue, tag, &stream);
        }
        else
        {
            llvm::raw_os_ostream llvmStream(stream);
            auto llvmValue = std::get<Emittable>(value.GetUnderlyingData()).GetDataAs<LLVMValue>();
            emitters::DebugDump(llvmValue, tag, &llvmStream);
        }
    }

    Value LLVMContext::IntrinsicCall(FunctionDeclaration intrinsic, std::vector<Value> args)
    {
        static std::unordered_map<FunctionDeclaration, std::function<Value(std::vector<Value>)>> intrinsics =
            {
                { AbsFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetAbsFunction) },
                { CosFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetCosFunction) },
                { ExpFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetExpFunction) },
                { LogFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetLogFunction) },
                { MaxNumFunctionDeclaration, MaxMinIntrinsicFunction(GetFunctionEmitter(), MaxMinIntrinsic::Max) },
                { MinNumFunctionDeclaration, MaxMinIntrinsicFunction(GetFunctionEmitter(), MaxMinIntrinsic::Min) },
                { PowFunctionDeclaration, PowFunctionIntrinsic(GetFunctionEmitter()) },
                { SinFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetSinFunction) },
                { SqrtFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetSqrtFunction) },
                { TanhFunctionDeclaration, SimpleNumericalFunctionIntrinsic(GetFunctionEmitter(), &IRRuntime::GetTanhFunction) }
            };

        if (std::all_of(args.begin(), args.end(), [](const auto& value) { return value.IsConstant(); }))
        {
            // Compute context can handle intrinsic calls with constant data
            return *_computeContext.Call(intrinsic, args);
        }

        std::vector<Value> emittableArgs;
        emittableArgs.reserve(args.size());
        std::transform(args.begin(), args.end(), std::back_inserter(emittableArgs), [this](const auto& value) { return EnsureEmittable(value); });

        if (auto it = intrinsics.find(intrinsic); it != intrinsics.end())
        {
            return it->second(emittableArgs);
        }

        throw LogicException(LogicExceptionErrors::notImplemented);
    }

    std::optional<Value> LLVMContext::EmitExternalCall(FunctionDeclaration externalFunc, std::vector<Value> args)
    {
        const auto& argTypes = externalFunc.GetParameterTypes();
        if (args.size() != argTypes.size())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        auto& irEmitter = _emitter.GetIREmitter();
        auto& fnEmitter = GetFunctionEmitter();

        const auto& returnType = externalFunc.GetReturnType();
        auto resultType = [&] {
            if (returnType)
            {
                return ValueTypeToLLVMType(irEmitter, { returnType->GetBaseType(), returnType->PointerLevel() });
            }
            else
            {
                return ValueTypeToLLVMType(irEmitter, { ValueType::Void, 0 });
            }
        }();

        std::vector<LLVMType> paramTypes(argTypes.size());
        std::transform(argTypes.begin(), argTypes.end(), paramTypes.begin(), [&](const auto& value) {
            return ValueTypeToLLVMType(irEmitter, { value.GetBaseType(), value.PointerLevel() });
        });

        // Create external function declaration
        const auto& fnName = externalFunc.GetFunctionName();
        auto fnType = llvm::FunctionType::get(resultType, paramTypes, false);
        _emitter.DeclareFunction(fnName, fnType);
        auto fn = _emitter.GetFunction(fnName);

        // as a first approximation, if the corresponding arg type has a pointer level that's one less
        // than the passed in value, we dereference it. if it's the same, we pass it in as is. if it's anything else,
        // throw. this logic may not be sufficient for future use cases.
        std::vector<LLVMValue> argValues;
        argValues.reserve(args.size());
        for (auto idx = 0u; idx < args.size(); ++idx)
        {
            auto arg = EnsureEmittable(args[idx]);
            const auto& type = argTypes[idx];
            if (arg.GetBaseType() != type.GetBaseType())
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }

            if (arg.PointerLevel() == type.PointerLevel())
            {
                argValues.push_back(ToLLVMValue(arg));
            }
            else if (arg.PointerLevel() == (type.PointerLevel() + 1))
            {
                argValues.push_back(fnEmitter.ValueAt(ToLLVMValue(arg), 0));
            }
            else
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }
        }

        auto resultValue = fnEmitter.Call(fn, argValues);
        auto result = returnType;
        if (result)
        {
            if (result->PointerLevel() == 0)
            {
                result = value::Allocate(result->GetBaseType(), ScalarLayout);
                fnEmitter.SetValueAt(ToLLVMValue(*result), 0, resultValue);
            }
            else
            {
                result->SetData(Emittable{ resultValue });
            }
        }

        return result;
    }

    bool LLVMContext::TypeCompatible(Value value1, Value value2)
    {
        return value1.GetBaseType() == value2.GetBaseType();
    }

    std::string LLVMContext::GetScopeAdjustedName(GlobalAllocationScope scope, std::string name) const
    {
        switch (scope)
        {
        case GlobalAllocationScope::Global:
            return GetGlobalScopedName(name);
        case GlobalAllocationScope::Function:
            return GetCurrentFunctionScopedName(name);
        }

        throw LogicException(LogicExceptionErrors::illegalState);
    }

    std::string LLVMContext::GetGlobalScopedName(std::string name) const
    {
        return _emitter.GetModuleName() + "_" + name;
    }

    std::string LLVMContext::GetCurrentFunctionScopedName(std::string name) const
    {
        if (_functionStack.empty())
        {
            throw LogicException(LogicExceptionErrors::illegalState);
        }

        return GetGlobalScopedName(GetFunctionEmitter().GetFunctionName() + "_" + name);
    }

    IRFunctionEmitter& LLVMContext::GetFunctionEmitter() const { return _functionStack.top().get(); }

    Value LLVMContext::PromoteConstantData(Value value)
    {
        assert(value.IsConstant() && value.IsDefined() && !value.IsEmpty());

        const auto& constantData = _computeContext.GetConstantData(value);

        ptrdiff_t offset = 0;
        LLVMValue llvmValue = std::visit(
            [this, &value, &offset](auto&& data) -> LLVMValue {
                using Type = std::decay_t<decltype(data)>;
                using DataType = typename Type::value_type;

                auto ptrData = std::get<DataType*>(value.GetUnderlyingData());
                offset = ptrData - data.data();

                if (_functionStack.empty())
                {
                    std::string globalName =
                        GetGlobalScopedName("_"s + std::to_string(_promotedConstantStack.top().size()));

                    llvm::GlobalVariable* globalVariable = nullptr;
                    if constexpr (std::is_same_v<DataType, Boolean>)
                    {
                        globalVariable =
                            _emitter.GlobalArray(globalName, std::vector<uint8_t>(data.begin(), data.end()));
                    }
                    else
                    {
                        globalVariable = _emitter.GlobalArray(globalName, data);
                    }

                    return globalVariable;
                }
                else
                {
                    auto& fn = GetFunctionEmitter();

                    std::string globalName = GetCurrentFunctionScopedName("_"s + std::to_string(_promotedConstantStack.top().size()));

                    llvm::GlobalVariable* globalVariable = nullptr;
                    if constexpr (std::is_same_v<DataType, Boolean>)
                    {
                        globalVariable =
                            _emitter.GlobalArray(globalName, std::vector<uint8_t>(data.begin(), data.end()));
                    }
                    else
                    {
                        globalVariable = _emitter.GlobalArray(globalName, data);
                    }

                    auto varType =
                        GetVariableType<std::conditional_t<std::is_same_v<DataType, Boolean>, bool, DataType>>();

                    LLVMValue newValue = fn.Variable(varType, data.size());
                    fn.MemoryCopy<DataType>(globalVariable, newValue, static_cast<int>(data.size()));

                    return newValue;
                }
            },
            constantData);

        _promotedConstantStack.top().push_back({ &constantData, llvmValue });

        auto& irEmitter = _emitter.GetIREmitter();
        auto llvmOffset = irEmitter.Literal(static_cast<int>(offset));
        if (auto globalVariable = llvm::dyn_cast<llvm::GlobalVariable>(llvmValue))
        {
            llvmValue = irEmitter.PointerOffset(globalVariable, llvmOffset);
        }
        else
        {
            llvmValue = irEmitter.PointerOffset(llvmValue, llvmOffset);
        }

        Value newValue = value;
        newValue.SetData(Emittable{ llvmValue });

        return newValue;
    }

    std::optional<LLVMContext::PromotedConstantDataDescription> LLVMContext::HasBeenPromoted(Value value) const
    {
        if (!value.IsDefined() || value.IsEmpty() || !value.IsConstant())
        {
            return std::nullopt;
        }

        const auto& constantData = _computeContext.GetConstantData(value);
        const auto& promotedStack = _promotedConstantStack.top();

        if (auto it = std::find_if(promotedStack.begin(),
                                   promotedStack.end(),
                                   [&constantData](const auto& desc) { return desc.data == &constantData; });
            it != promotedStack.end())
        {
            return *it;
        }
        else
        {
            return std::nullopt;
        }
    }

    Value LLVMContext::Realize(Value value) const
    {
        if (auto desc = HasBeenPromoted(value); !desc)
        {
            return value;
        }
        else
        {
            const auto& promotionalDesc = *desc;
            auto offset = std::visit(
                [&value](auto&& data) -> ptrdiff_t {
                    using Type = std::decay_t<decltype(data)>;
                    using DataType = typename Type::value_type;

                    auto ptrData = std::get<DataType*>(value.GetUnderlyingData());

                    return ptrData - data.data();
                },
                *promotionalDesc.data);

            auto& fn = GetFunctionEmitter();
            auto emittable = promotionalDesc.realValue;

            Value newValue = value;
            newValue.SetData(Emittable{ fn.PointerOffset(emittable.GetDataAs<LLVMValue>(), static_cast<int>(offset)) });

            return newValue;
        }
    }

    Value LLVMContext::EnsureEmittable(Value value)
    {
        if (!value.IsConstant())
        {
            return value;
        }
        else if (Value newValue = Realize(value); !newValue.IsConstant())
        {
            return newValue;
        }
        else
        {
            return PromoteConstantData(newValue);
        }
    }

} // namespace value
} // namespace ell
