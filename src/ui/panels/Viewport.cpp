#include "panels/Viewport.hpp"

#include "ui/UIContext.hpp"
#include "core/Log.hpp"
#include "editor/Input.hpp"
#include "ui/events/UIEvents.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QWindow>

namespace neurus {

Viewport::Viewport(QWidget* parent)
	: UIPanel(PanelType::Viewport, nullptr, parent)
{
	// Only the viewport needs a native surface. Keeping the ADS containers
	// non-native lets Qt resize the surrounding dock layout in its backing store.
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	// Vulkan presents directly to this child window. Exclude it from Qt's
	// backing-store flush when a splitter invalidates the dock layout.
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NoSystemBackground);
	setAutoFillBackground(false);

	setFocusPolicy(Qt::StrongFocus);

	// Deliver moves even when no button is held.
	setMouseTracking(true);
	qApp->installEventFilter(this);

	m_resizeTimer.setSingleShot(true);
	m_resizeTimer.setTimerType(Qt::PreciseTimer);
	connect(&m_resizeTimer, &QTimer::timeout, this, [this] {
		// The splitter receives the release even when its final size is unchanged.
		if (m_resizeWithMouse && QApplication::mouseButtons().testFlag(Qt::LeftButton))
			m_resizeTimer.start();
		else
			emit resizeSettled();
	});
}

Viewport::~Viewport() = default;

bool Viewport::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::Move && isVisible())
	{
		// At minimum size the splitter can translate a non-native dock ancestor
		// without moving or resizing the viewport widget inside it. Keep its
		// native child window at the corresponding position in the native parent.
		auto* moved = qobject_cast<QWidget*>(watched);
		if (moved && !moved->internalWinId() && moved->isAncestorOf(this))
		{
			// A left-edge drag moves the dock before its layout shrinks the
			// viewport. Let Qt apply that move and resize together if moving
			// the current native window would put it outside the new dock.
			auto* native = windowHandle();
			if (native && moved->rect().contains(QRect(mapTo(moved, QPoint{}), native->size())))
				SyncNativePosition();
		}
	}
	return UIPanel::eventFilter(watched, event);
}

void Viewport::showEvent(QShowEvent* event)
{
	UIPanel::showEvent(event);
	SyncNativePosition();
}

void Viewport::SyncNativePosition()
{
	auto* native = windowHandle();
	auto* parent = nativeParentWidget();
	if (!native || !parent || native->parent() != parent->windowHandle())
		return;

	const QPoint position = mapTo(parent, QPoint{});
	if (native->position() != position)
		native->setPosition(position);
}

void Viewport::Refresh(const UIContext& /*ctx*/)
{
	// Nothing to refresh: the viewport holds no scene state.
}

void Viewport::paintEvent(QPaintEvent* /*event*/)
{
	// A converged renderer is idle until the exposed image needs a redraw.
	emit exposed();
}

void Viewport::resizeEvent(QResizeEvent* event)
{
	if (!event->size().isEmpty())
	{
		m_resizeWithMouse = QApplication::mouseButtons().testFlag(Qt::LeftButton);
		constexpr int kReleasePollMs = 16;
		constexpr int kResizeQuietMs = 120;
		m_resizeTimer.start(m_resizeWithMouse ? kReleasePollMs : kResizeQuietMs);
		emit resized(event->size().width(), event->size().height(), m_resizeWithMouse);
	}

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

	// Clicking another panel is the usual way the viewport is invalidated while
	// the renderer is converged, so ask for one fresh present as well. Cheap:
	// the Editor only queues an overlay redraw.
	emit exposed();

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
