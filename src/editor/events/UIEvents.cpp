#include "UIEvents.hpp"

namespace neurus {

UIEvents& UIEvents::instance()
{
	static UIEvents bus;
	return bus;
}

QString UIEvents::gpuName() const
{
	return evt_gpuName;
}

void UIEvents::setGpuName(const QString& name)
{
	if (evt_gpuName != name)
	{
		evt_gpuName = name;
		emit gpuNameChanged();
	}
}

} // namespace neurus
