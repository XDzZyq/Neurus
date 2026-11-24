/**
 * @file Environment.cpp
 * @brief Implementation of Environment - CPU-only IBL environment map source.
 */

#include "scene/Environment.hpp"
#include "asset/data/ImageData.hpp"
#include "core/Log.hpp"

namespace neurus
{

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

Environment::Environment()
	: ObjectID(Type, "Environment")
{
}

Environment::Environment(RestoredId id)
	: ObjectID(id, Type, "Environment")
{
}

Environment::Environment(std::shared_ptr<ImageData> data)
{
	o_type = ObjectID::GOType::GO_ENVIR;
	o_name = "Environment";
	SetEquirectData(std::move(data));
}

Environment::~Environment() = default;

void Environment::SetEquirectData(std::shared_ptr<ImageData> data)
{
	o_equirectData = std::move(data);
}

} // namespace neurus
