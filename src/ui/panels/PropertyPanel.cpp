#include "panels/PropertyPanel.h"

#include "Icons.h"
#include "UIContext.h"
#include "ui/utils/I18n.h"
#include "presets/CameraProperties.h"
#include "presets/EnvironmentProperties.h"
#include "presets/LightProperties.h"
#include "presets/MeshProperties.h"
#include "items/Vec3Spin.h"

#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "scene/ObjectID.h"
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"

#include <QGridLayout>

namespace neurus {

// =========================================================================
// Constructor
// =========================================================================

PropertyPanel::PropertyPanel(QWidget* parent)
	: UIPanel(PanelType::PropertyPanel, nullptr, parent)
{
	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(8, 8, 8, 8);

	BuildHeader();
	mainLayout->addWidget(m_headerWidget);

	BuildTransformEditor();
	mainLayout->addWidget(m_transformGroup);

	BuildTypeSubpanels();
	mainLayout->addWidget(m_cameraProps);
	mainLayout->addWidget(m_meshProps);
	mainLayout->addWidget(m_lightProps);
	mainLayout->addWidget(m_envProps);

	// Text is filled in by the Retranslate() call at the end of this ctor.
	m_emptyLabel = new QLabel();
	m_emptyLabel->setAlignment(Qt::AlignCenter);
	QFont emptyFont = m_emptyLabel->font();
	emptyFont.setPointSize(emptyFont.pointSize() + 1);
	m_emptyLabel->setFont(emptyFont);
	m_emptyLabel->setStyleSheet("QLabel { color: #888; }");
	mainLayout->addWidget(m_emptyLabel);

	mainLayout->addStretch();

	m_emptyLabel->setVisible(true);
	m_headerWidget->setVisible(false);
	m_transformGroup->setEnabled(false);

	// Apply the active language (transform labels were built in English).
	Retranslate();
}

// =========================================================================
// Retranslate - re-apply transform labels / empty-state text
// =========================================================================

void PropertyPanel::Retranslate()
{
	auto& i18n = I18n::instance();

	m_emptyLabel->setText(i18n.translate("No object selected"));
	m_transformGroup->setTitle(i18n.translate("Transform"));
	m_posLabel->setText(i18n.translate("Position"));
	m_rotLabel->setText(i18n.translate("Rotation"));
	m_sclLabel->setText(i18n.translate("Scale"));
	m_resetBtn->setText(i18n.translate("Reset Transform"));
	m_resetBtn->setToolTip(
		i18n.translateCtx("Reset position, rotation, and scale to identity values.", "Tooltip"));

	// Type-specific subpanels.
	m_cameraProps->Retranslate();
	m_meshProps->Retranslate();
	m_lightProps->Retranslate();
	m_envProps->Retranslate();
}

// =========================================================================
// BuildHeader — icon + name row
// =========================================================================

void PropertyPanel::BuildHeader()
{
	m_headerWidget = new QWidget();
	auto* row = new QHBoxLayout(m_headerWidget);
	row->setContentsMargins(0, 0, 0, 4);
	row->setSpacing(6);

	m_iconLabel = new QLabel();
	m_iconLabel->setFixedSize(20, 20);
	row->addWidget(m_iconLabel);

	m_nameLabel = new QLabel();
	QFont f = m_nameLabel->font();
	f.setBold(true);
	m_nameLabel->setFont(f);
	m_nameLabel->setWordWrap(true);
	row->addWidget(m_nameLabel, 1);
}

// =========================================================================
// Refresh — read active object from scene selections, update UI
// =========================================================================

void PropertyPanel::Refresh(const UIContext& ctx)
{
	const Scene* scene = static_cast<const Scene*>(ctx.editor.scene);
	if (!scene)
	{
		SetEnabled(false);
		m_activeObjectId = 0;
		return;
	}

	const ObjectID* activeObj = scene->selections.GetActiveObject();
	if (!activeObj)
	{
		SetEnabled(false);
		m_activeObjectId = 0;
		return;
	}

	int objectId = activeObj->GetObjectID();

	// --- Header: icon + name (lazy: only when the active UID changed) ---
	if (m_activeObjectId != objectId){
		m_iconLabel->setPixmap(Icons::ObjectIcon(static_cast<int>(activeObj->o_type)).pixmap(20, 20));
		m_nameLabel->setText(QString::fromStdString(activeObj->o_name));
		m_activeObjectId = objectId;
	}

	// --- Transform ---
	auto* obj = const_cast<ObjectID*>(activeObj);
	void* transformPtr = obj->GetTransform();
	if (transformPtr)
	{
		auto* xform = static_cast<Transform3D*>(transformPtr);
		const glm::vec3& pos = xform->GetPosition();
		const glm::vec3& rot = xform->GetRotation();
		const glm::vec3& scl = xform->GetScale();

		// Vec3Spin::setValue handles dirty-check internally
		m_posSpin->setValue(pos.x, pos.y, pos.z);
		m_rotSpin->setValue(rot.x, rot.y, rot.z);
		m_sclSpin->setValue(scl.x, scl.y, scl.z);

		SetEnabled(true);
	}
	else
	{
		SetEnabled(false);
	}

	// --- Type-specific subpanel ---
	ShowTypeSubpanel(static_cast<int>(activeObj->o_type));
	switch (activeObj->o_type)
	{
	case ObjectID::GOType::GO_CAM:
	{
		auto it = scene->cam_list.find(objectId);
		if (it != scene->cam_list.end())
		{
			auto* cam = it->second.get();
			m_cameraProps->setObjectId(objectId);
			m_cameraProps->setTarget(cam->cam_tar);
			m_cameraProps->setFov(cam->cam_pers);
		}
		break;
	}
	case ObjectID::GOType::GO_MESH:
	{
		auto it = scene->mesh_list.find(objectId);
		if (it != scene->mesh_list.end())
		{
			auto* mesh = it->second.get();
			m_meshProps->setObjectId(objectId);
			// Path is owned by the pooled MeshData (data layer).
			m_meshProps->setMeshPath(mesh->o_mesh ? mesh->o_mesh->GetPath() : "");
			m_meshProps->setShadowEnabled(mesh->using_shadow);
			m_meshProps->setMaterialEnabled(mesh->using_material);
		}
		break;
	}
	case ObjectID::GOType::GO_LIGHT:
	case ObjectID::GOType::GO_POLYLIGHT:
	{
		auto it = scene->light_list.find(objectId);
		if (it != scene->light_list.end())
		{
			auto* light = it->second.get();
			m_lightProps->setObjectId(objectId);
			m_lightProps->setLightType(Light::ParseLightName(light->light_type).second);
			m_lightProps->setPower(light->light_power);
			m_lightProps->setRadius(light->light_radius);
			m_lightProps->setShadowEnabled(light->use_shadow);
			m_lightProps->setCutoff(light->spot_cutoff);
			m_lightProps->setOuterCutoff(light->spot_outer_cutoff);
			m_lightProps->setSpotConeVisible(light->light_type == LightType::SPOTLIGHT);
		}
		break;
	}
	case ObjectID::GOType::GO_ENVIR:
	{
		auto it = scene->env_list.find(objectId);
		if (it != scene->env_list.end())
		{
			auto* env = it->second.get();
			m_envProps->setObjectId(objectId);
			m_envProps->setIntensity(env->GetIntensity());
			m_envProps->setRotation(env->GetRotation());
			// Path is owned by the pooled ImageData (data layer).
			auto eqData = env->GetEquirectData();
			m_envProps->setEquirectPath(eqData ? eqData->GetPath() : "");
		}
		break;
	}
	default:
		break;
	}
}

// =========================================================================
// BuildTransformEditor — QGroupBox with Vec3Spin rows + Reset button
// =========================================================================

void PropertyPanel::BuildTransformEditor()
{
	m_transformGroup = new QGroupBox("Transform");
	m_transformGroup->setCheckable(false);

	auto* grid = new QGridLayout(m_transformGroup);
	grid->setContentsMargins(10, 16, 10, 10);
	grid->setHorizontalSpacing(6);
	grid->setVerticalSpacing(6);

	auto makeAxisLabel = [](const QString& text, const QString& color) {
		auto* lbl = new QLabel(text);
		lbl->setAlignment(Qt::AlignCenter);
		lbl->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(color));
		return lbl;
	};

	// Row 0: axis column headers (X=red, Y=green, Z=blue)
	grid->addWidget(new QLabel(""), 0, 0);
	grid->addWidget(makeAxisLabel("X", "#e74c3c"), 0, 1);
	grid->addWidget(makeAxisLabel("Y", "#2ecc71"), 0, 2);
	grid->addWidget(makeAxisLabel("Z", "#3498db"), 0, 3);

	// Row 1: Position — Vec3Spin spans columns 1–3
	m_posLabel = new QLabel("Position");
	grid->addWidget(m_posLabel, 1, 0);
	m_posSpin = new Vec3Spin(-100000.0, 100000.0, 0.01, 2, QString());
	grid->addWidget(m_posSpin, 1, 1, 1, 3);

	// Row 2: Rotation — Vec3Spin spans columns 1–3
	m_rotLabel = new QLabel("Rotation");
	grid->addWidget(m_rotLabel, 2, 0);
	m_rotSpin = new Vec3Spin(-360.0, 360.0, 1.0, 1, "\u00B0");
	grid->addWidget(m_rotSpin, 2, 1, 1, 3);

	// Row 3: Scale — Vec3Spin spans columns 1–3
	m_sclLabel = new QLabel("Scale");
	grid->addWidget(m_sclLabel, 3, 0);
	m_sclSpin = new Vec3Spin(0.001, 1000.0, 0.1, 3, QString());
	m_sclSpin->setValue(1.0, 1.0, 1.0);  // initial identity
	grid->addWidget(m_sclSpin, 3, 1, 1, 3);

	// Row 4: Reset button — spans all 4 columns
	m_resetBtn = new QPushButton("Reset Transform");
	m_resetBtn->setToolTip("Reset position, rotation, and scale to identity values.");
	grid->addWidget(m_resetBtn, 4, 0, 1, 4, Qt::AlignCenter);

	// --- Signal wiring ---
	// Each Vec3Spin emits valueChanged(x, y, z); one signal per transform component.
	QObject::connect(m_posSpin, &Vec3Spin::valueChanged, this,
		[this](float x, float y, float z) {
			if (m_activeObjectId == 0) return;
			emit positionChanged(PositionChanged{m_activeObjectId, x, y, z});
		});

	QObject::connect(m_rotSpin, &Vec3Spin::valueChanged, this,
		[this](float x, float y, float z) {
			if (m_activeObjectId == 0) return;
			emit rotationChanged(RotationChanged{m_activeObjectId, x, y, z});
		});

	QObject::connect(m_sclSpin, &Vec3Spin::valueChanged, this,
		[this](float x, float y, float z) {
			if (m_activeObjectId == 0) return;
			emit scaleChanged(ScaleChanged{m_activeObjectId, x, y, z});
		});

	// --- Reset button ---
	QObject::connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
		m_posSpin->setValue(0.0, 0.0, 0.0);
		m_rotSpin->setValue(0.0, 0.0, 0.0);
		m_sclSpin->setValue(1.0, 1.0, 1.0);

		if (m_activeObjectId != 0)
		{
			emit positionChanged(PositionChanged{m_activeObjectId, 0.0f, 0.0f, 0.0f});
			emit rotationChanged(RotationChanged{m_activeObjectId, 0.0f, 0.0f, 0.0f});
			emit scaleChanged(ScaleChanged{m_activeObjectId, 1.0f, 1.0f, 1.0f});
		}
	});
}

// =========================================================================
// SetEnabled
// =========================================================================

void PropertyPanel::SetEnabled(bool enabled)
{
	m_headerWidget->setVisible(enabled);
	m_transformGroup->setVisible(enabled);
	m_transformGroup->setEnabled(enabled);
	m_emptyLabel->setVisible(!enabled);

	if (!enabled)
	{
		ShowTypeSubpanel(static_cast<int>(ObjectID::GOType::NONE_GO));
	}
}

// =========================================================================
// BuildTypeSubpanels — create type-specific editors (each owns its QGroupBox)
// =========================================================================

void PropertyPanel::BuildTypeSubpanels()
{
	m_cameraProps = new CameraProperties(this);
	m_meshProps   = new MeshProperties(this);
	m_lightProps  = new LightProperties(this);
	m_envProps    = new EnvironmentProperties(this);

	// --- Forward signals from subpanels to PropertyPanel signals ---

	// Camera
	QObject::connect(m_cameraProps, &CameraProperties::targetChanged, this,
		[this](int /*objectId*/, float x, float y, float z) {
			emit cameraTargetChanged(CameraTargetChanged{m_activeObjectId, x, y, z});
		});
	QObject::connect(m_cameraProps, &CameraProperties::fovChanged, this,
		[this](int /*objectId*/, float fov) {
			emit cameraFovChanged(CameraFovChanged{m_activeObjectId, fov});
		});

	// Mesh
	QObject::connect(m_meshProps, &MeshProperties::shadowChanged, this,
		[this](int /*objectId*/, bool enabled) {
			emit meshShadowChanged(MeshShadowChanged{m_activeObjectId, enabled});
		});
	QObject::connect(m_meshProps, &MeshProperties::materialChanged, this,
		[this](int /*objectId*/, bool enabled) {
			emit meshMaterialChanged(MeshMaterialChanged{m_activeObjectId, enabled});
		});

	// Light
	QObject::connect(m_lightProps, &LightProperties::powerChanged, this,
		[this](int /*objectId*/, float power) {
			emit lightPowerChanged(LightPowerChanged{m_activeObjectId, power});
		});
	QObject::connect(m_lightProps, &LightProperties::radiusChanged, this,
		[this](int /*objectId*/, float radius) {
			emit lightRadiusChanged(LightRadiusChanged{m_activeObjectId, radius});
		});
	QObject::connect(m_lightProps, &LightProperties::shadowChanged, this,
		[this](int /*objectId*/, bool enabled) {
			emit lightShadowChanged(LightShadowChanged{m_activeObjectId, enabled});
		});
	QObject::connect(m_lightProps, &LightProperties::cutoffChanged, this,
		[this](int /*objectId*/, float cosine) {
			emit lightCutoffChanged(LightCutoffChanged{m_activeObjectId, cosine});
		});
	QObject::connect(m_lightProps, &LightProperties::outerCutoffChanged, this,
		[this](int /*objectId*/, float cosine) {
			emit lightOuterCutoffChanged(LightOuterCutoffChanged{m_activeObjectId, cosine});
		});

	// Environment
	QObject::connect(m_envProps, &EnvironmentProperties::intensityChanged, this,
		[this](int /*objectId*/, float intensity) {
			emit envIntensityChanged(EnvironmentIntensityChanged{m_activeObjectId, intensity});
		});
	QObject::connect(m_envProps, &EnvironmentProperties::rotationChanged, this,
		[this](int /*objectId*/, float rotation) {
			emit envRotationChanged(EnvironmentRotationChanged{m_activeObjectId, rotation});
		});

	// Start with all hidden
	ShowTypeSubpanel(static_cast<int>(ObjectID::GOType::NONE_GO));
}

// =========================================================================
// ShowTypeSubpanel — show only the subpanel matching the given GOType
// =========================================================================

void PropertyPanel::ShowTypeSubpanel(int goType)
{
	m_cameraProps->setVisible(goType == static_cast<int>(ObjectID::GOType::GO_CAM));
	m_meshProps->setVisible(goType == static_cast<int>(ObjectID::GOType::GO_MESH));
	m_lightProps->setVisible(goType == static_cast<int>(ObjectID::GOType::GO_LIGHT) ||
	                         goType == static_cast<int>(ObjectID::GOType::GO_POLYLIGHT));
	m_envProps->setVisible(goType == static_cast<int>(ObjectID::GOType::GO_ENVIR));
}

} // namespace neurus
