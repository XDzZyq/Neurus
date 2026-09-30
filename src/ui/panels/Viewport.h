#pragma once

#include "platform/PlatformSurface.h"
#include "UIPanel.h"

#include <glm/glm.hpp>

#include "editor/events/InputEvents.h"
#include "editor/events/SceneEvents.h"

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace neurus {

/**
 * @brief A QWidget subclass that exposes a native window handle
 *        for Vulkan surface creation.
 *
 * Sets WA_NativeWindow and WA_OpaquePaintEvent attributes to ensure the widget
 * has a dedicated native window handle and Qt does not draw a background behind
 * the Vulkan content. Overrides paintEvent() to be a no-op - all rendering is
 * handled by Vulkan.
 *
 * The widget is focusable (Qt::StrongFocus) to receive keyboard input.
 * Emits resized(int, int) when the widget is resized, allowing the renderer
 * to recreate the swapchain with the new dimensions.
 *
 * Mouse events are translated to typed event structs and emitted as Qt signals
 * (mouseMoved, mousePressed, mouseReleased, mouseScrolled). The Viewport tracks
 * its own button state and mouse position internally; it no longer calls
 * Input::Record*().
 *
 * @note This class owns no Vulkan resources. It provides the native window
 *       handle only; surface creation is the Renderer's responsibility.
 */
class Viewport : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::Viewport;

	/**
	 * @brief Constructs a Viewport with native window attributes.
	 * @param parent Optional parent widget.
	 */
	explicit Viewport(QWidget* parent = nullptr);

	/**
	 * @brief Refreshes the panel from a UIContext snapshot.
	 *
	 * The viewport is pure input forwarding + Vulkan rendering; it holds no
	 * scene state. Mouse/Delete events are pure intents - the Editor wraps
	 * the active scene and forwards dedicated events to controllers.
	 *
	 * @param ctx Read-only UI context (scene snapshot).
	 */
	void Refresh(const UIContext& ctx) override;

	/** @brief Default destructor. */
	~Viewport() override;

	Viewport(const Viewport&) = delete;
	Viewport& operator=(const Viewport&) = delete;

	/**
	 * @brief Returns the native window handle.
	 * @return NativeWindowHandle of the underlying native window.
	 *
	 * The handle is obtained via winId() and is valid once the widget is
	 * shown or realize()d. Pass this to the PlatformSurface for surface
	 * creation.
	 */
	NativeWindowHandle hwnd() const { return reinterpret_cast<NativeWindowHandle>(winId()); }

signals:
	/**
	 * @brief Emitted when the widget is resized.
	 * @param width New width in pixels.
	 * @param height New height in pixels.
	 *
	 * Connect this to the Renderer to trigger swapchain recreation with
	 * the updated dimensions.
	 */
	void resized(int width, int height);

	/**
	 * @brief Emitted when the mouse cursor moves over the viewport.
	 * @param event MouseMoveEvent with position, delta, modifiers, and button state.
	 */
	void mouseMoved(const neurus::MouseMoveEvent& event);

	/**
	 * @brief Emitted when a mouse button is pressed inside the viewport.
	 * @param event MousePressEvent with button, position, and modifiers.
	 */
	void mousePressed(const neurus::MousePressEvent& event);

	/**
	 * @brief Emitted when a mouse button is released inside the viewport.
	 * @param event MouseReleaseEvent with button, position, and modifiers.
	 */
	void mouseReleased(const neurus::MouseReleaseEvent& event);

	/**
	 * @brief Emitted when the mouse wheel is scrolled over the viewport.
	 * @param event MouseScrollEvent with delta, position, modifiers, and button state.
	 */
	void mouseScrolled(const neurus::MouseScrollEvent& event);

	/**
	 * @brief Emitted when the Delete key is pressed while the viewport has focus.
	 * @note Pure intent - the Editor wraps the active scene and deletes ALL selected objects.
	 */
	void deleteRequested(const neurus::DeleteRequested& event);

	/**
	 * @brief Emitted when a key the editor handles is pressed in the viewport.
	 * @param event KeyPressEvent carrying an Input::Key and the modifiers.
	 *
	 * Only keys Input::GetKey() recognises are forwarded, and auto-repeat is
	 * filtered here at the Qt boundary so a held key arms a modal operator once.
	 * Deliberately not a QShortcut: a Qt::WindowShortcut on G/R/S/X/Y/Z would
	 * steal those letters from the shader editor and from every property
	 * spinbox, and cannot filter auto-repeat.
	 */
	void keyPressed(const neurus::KeyPressEvent& event);

	/**
	 * @brief Emitted when the viewport loses keyboard focus.
	 * @param event ViewportFocusLost (empty - the fact is the whole payload).
	 *
	 * A modal transform gesture is driven by viewport keys and the viewport
	 * cursor, so clicking into another panel mid-gesture must abort it rather
	 * than leave an invisible operator armed against a widget that no longer
	 * receives its keys.
	 */
	void focusLost(const neurus::ViewportFocusLost& event);

protected:
	/**
	 * @brief Override to prevent Qt from drawing a background.
	 * @param event The paint event (ignored).
	 *
	 * All rendering for this widget is done by Vulkan. Qt's paint system
	 * should not draw anything in this widget's area.
	 */
	void paintEvent(QPaintEvent* event) override;

	/**
	 * @brief Handles resize events and emits the resized signal.
	 * @param event The resize event containing new dimensions.
	 */
	void resizeEvent(QResizeEvent* event) override;

	/**
	 * @brief Handles keyboard input.
	 * @param event The key event.
	 *
	 * Delete emits deleteRequested; F12 triggers a screenshot via UIEvents and
	 * Ctrl+F12 an attachment dump. Everything Input::GetKey() recognises is
	 * translated and emitted as keyPressed, accepted so ADS and the QMainWindow
	 * menus do not also see it. All remaining keys chain to the base class.
	 */
	void keyPressEvent(QKeyEvent* event) override;

	/**
	 * @brief Handles key release events.
	 * @param event The key event.
	 *
	 * Chains to the base class. There is deliberately no KeyReleaseEvent in the
	 * editor's event set: a modal operator is closed by a confirm or a cancel,
	 * never by letting go of the key that armed it.
	 */
	void keyReleaseEvent(QKeyEvent* event) override;

	/**
	 * @brief Emits focusLost so a live modal gesture can abort.
	 * @param event The focus event.
	 */
	void focusOutEvent(QFocusEvent* event) override;

	/**
	 * @brief Handles mouse movement and emits the mouseMoved signal.
	 * @param event The mouse event.
	 *
	 * Computes delta from the last recorded position, translates Qt types
	 * via Input helpers, builds a MouseMoveEvent, and emits mouseMoved().
	 * Mouse tracking is enabled so movement is captured even without a
	 * button held.
	 */
	void mouseMoveEvent(QMouseEvent* event) override;

	/**
	 * @brief Handles mouse button presses and emits the mousePressed signal.
	 * @param event The mouse event.
	 *
	 * Resets m_lastPos to current position on press (zero delta for the
	 * next move), updates internal button state, translates via Input
	 * helpers, and emits mousePressed().
	 */
	void mousePressEvent(QMouseEvent* event) override;

	/**
	 * @brief Handles mouse button releases and emits the mouseReleased signal.
	 * @param event The mouse event.
	 *
	 * Clears internal button state, translates via Input helpers, and
	 * emits mouseReleased().
	 */
	void mouseReleaseEvent(QMouseEvent* event) override;

	/**
	 * @brief Handles mouse wheel scrolling and emits the mouseScrolled signal.
	 * @param event The wheel event.
	 *
	 * Converts angleDelta to notches (~+/-1 per detent), translates via
	 * Input helpers, and emits mouseScrolled().
	 */
	void wheelEvent(QWheelEvent* event) override;

private:
	QPointF m_lastPos;        ///< Last mouse position for delta computation.
	bool    m_leftHeld = false;   ///< Left mouse button held.
	bool    m_middleHeld = false; ///< Middle mouse button held.
	bool    m_rightHeld = false;  ///< Right mouse button held.
};

} // namespace neurus
