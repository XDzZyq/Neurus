#include "panels/Viewport.h"

#include "UIContext.h"
#include "core/Log.h"
#include "editor/Input.h"
#include "editor/events/UIEvents.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

namespace neurus {

Viewport::Viewport(QWidget* parent)
	: UIPanel(PanelType::Viewport, nullptr, parent)
{
	// Dedicated native window handle for Vulkan surface creation.
	setAttribute(Qt::WA_NativeWindow);

	// This widget paints all its pixels: no background fill is needed.
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NoSystemBackground);
	setAutoFillBackground(false);

	setFocusPolicy(Qt::StrongFocus);

	// Deliver moves even when no button is held.
	setMouseTracking(true);
}

Viewport::~Viewport() = default;

void Viewport::Refresh(const UIContext& /*ctx*/)
{
	// Nothing to refresh: the viewport holds no scene state.
}

void Viewport::paintEvent(QPaintEvent* /*event*/)
{
	// No-op; the override stops Qt from filling the widget background.
}

void Viewport::resizeEvent(QResizeEvent* event)
{
	// Publish the new dimensions so the swapchain can be recreated.
	emit resized(event->size().width(), event->size().height());

	QWidget::resizeEvent(event);
}

void Viewport::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Delete)
	{
		emit deleteRequested(DeleteRequested{});
		event->accept();
		return;
	}

	if (event->key() == Qt::Key_F12)
	{
		if (event->modifiers() == Qt::NoModifier)
		{
			NEURUS_LOG("[Screenshot] F12 pressed - requesting screenshot via UIEvents");
			UIEvents::instance().requestScreenshot();
			return;
		}
		else if (event->modifiers() == Qt::ControlModifier)
		{
			NEURUS_LOG("[Screenshot] Ctrl+F12 pressed - requesting attachment dump via UIEvents");
			UIEvents::instance().requestScreenshotAll();
			return;
		}
	}

	// Auto-repeat is filtered here, at the Qt boundary: a held key must arm an
	// operator once, and no downstream layer sees repeat information.
	if (!event->isAutoRepeat())
	{
		const Input::Key key = Input::GetKey(static_cast<uint32_t>(event->key()));
		if (key != Input::Key_Unknown)
		{
			emit keyPressed(KeyPressEvent{
				.key = key,
				.modifiers = Input::GetModifiers(static_cast<uint32_t>(event->modifiers().toInt()))
			});
			// Accepted so the keystroke stops here: unaccepted, it would walk up
			// to the window, where a menu mnemonic could claim it.
			event->accept();
			return;
		}
	}

	QWidget::keyPressEvent(event);
}

void Viewport::focusOutEvent(QFocusEvent* event)
{
	// A modal gesture reads viewport keys and the cursor, so focus moving to
	// another widget is a genuine abort. Losing frontmost activation is not:
	// an inactive app is delivered no mouse moves, so the gesture pauses and
	// resumes untouched. The controller ignores the signal when nothing is armed.
	const Qt::FocusReason reason = event->reason();
	if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason)
		emit focusLost(ViewportFocusLost{});

	QWidget::focusOutEvent(event);
}

void Viewport::keyReleaseEvent(QKeyEvent* event)
{
	QWidget::keyReleaseEvent(event);
}

void Viewport::mouseMoveEvent(QMouseEvent* event)
{
	const QPointF pos = event->position();
	const MouseMoveEvent evt{
		.position = Input::GetMousePos(static_cast<float>(pos.x()), static_cast<float>(pos.y())),
		.delta = Input::GetMousePos(static_cast<float>(pos.x() - m_lastPos.x()), static_cast<float>(pos.y() - m_lastPos.y())),
		.modifiers = Input::GetModifiers(static_cast<uint32_t>(event->modifiers().toInt())),
		.leftHeld = m_leftHeld,
		.middleHeld = m_middleHeld,
		.rightHeld = m_rightHeld
	};
	m_lastPos = pos;
	emit mouseMoved(evt);
	QWidget::mouseMoveEvent(event);
}

void Viewport::mousePressEvent(QMouseEvent* event)
{
	m_lastPos = event->position();  // Reset delta on press
	const auto btn = Input::GetMouseButton(static_cast<uint32_t>(event->button()));
	switch (btn)
	{
		case Input::MouseButton::Left:   m_leftHeld = true; break;
		case Input::MouseButton::Right:  m_rightHeld = true; break;
		case Input::MouseButton::Middle: m_middleHeld = true; break;
	}
	const MousePressEvent evt{
		.button = btn,
		.position = Input::GetMousePos(static_cast<float>(event->position().x()), static_cast<float>(event->position().y())),
		.modifiers = Input::GetModifiers(static_cast<uint32_t>(event->modifiers().toInt()))
	};
	emit mousePressed(evt);
	QWidget::mousePressEvent(event);
}

void Viewport::mouseReleaseEvent(QMouseEvent* event)
{
	const auto btn = Input::GetMouseButton(static_cast<uint32_t>(event->button()));
	switch (btn)
	{
		case Input::MouseButton::Left:   m_leftHeld = false; break;
		case Input::MouseButton::Right:  m_rightHeld = false; break;
		case Input::MouseButton::Middle: m_middleHeld = false; break;
	}
	const MouseReleaseEvent evt{
		.button = btn,
		.position = Input::GetMousePos(static_cast<float>(event->position().x()), static_cast<float>(event->position().y())),
		.modifiers = Input::GetModifiers(static_cast<uint32_t>(event->modifiers().toInt()))
	};
	emit mouseReleased(evt);
	QWidget::mouseReleaseEvent(event);
}

void Viewport::wheelEvent(QWheelEvent* event)
{
	const float notches = event->angleDelta().y() / 120.0f;
	const MouseScrollEvent evt{
		.delta = notches,
		.position = Input::GetMousePos(static_cast<float>(event->position().x()), static_cast<float>(event->position().y())),
		.modifiers = Input::GetModifiers(static_cast<uint32_t>(event->modifiers().toInt())),
		.leftHeld = m_leftHeld,
		.middleHeld = m_middleHeld,
		.rightHeld = m_rightHeld
	};
	emit mouseScrolled(evt);
	QWidget::wheelEvent(event);
}

} // namespace neurus
