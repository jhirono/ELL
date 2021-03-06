////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     TanhActivation.h (neural)
//  Authors:  James Devine
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Activation.h"

#include <math/include/Tensor.h>
#include <math/include/Vector.h>

#include <cmath>

namespace ell
{
namespace predictors
{
    namespace neural
    {
        /// <summary> Implements the hyperbolic tangent function: tanh(x) = 2 . sigmoid(2x) - 1 </summary>
        template <typename ElementType>
        class TanhActivation : public ActivationImpl<ElementType>
        {
        public:
            /// <summary> Returns the output as a function of the input. </summary>
            ///
            /// <param name="input"> The input value. </param>
            ElementType Apply(const ElementType input) const override;

            /// <summary> Gets the name of this type. </summary>
            ///
            /// <returns> The name of this type. </returns>
            static std::string GetTypeName() { return utilities::GetCompositeTypeName<ElementType>("TanhActivation"); }

            /// <summary> Gets the name of this type (for serialization). </summary>
            ///
            /// <returns> The name of this type. </returns>
            virtual std::string GetRuntimeTypeName() const override { return GetTypeName(); }

            /// <summary> Make a copy of this activation. </summary>
            ///
            /// <returns> The copy in a unique pointer. </param>
            std::unique_ptr<ActivationImpl<ElementType>> Copy() const override;
        };
    } // namespace neural
} // namespace predictors
} // namespace ell

#pragma region implementation

namespace ell
{
namespace predictors
{
    namespace neural
    {
        template <typename ElementType>
        ElementType TanhActivation<ElementType>::Apply(const ElementType input) const
        {
            return std::tanh(input);
        }

        template <typename ElementType>
        std::unique_ptr<ActivationImpl<ElementType>> TanhActivation<ElementType>::Copy() const
        {
            return std::make_unique<TanhActivation<ElementType>>();
        }
    } // namespace neural
} // namespace predictors
} // namespace ell

#pragma endregion implementation
