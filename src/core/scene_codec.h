#pragma once

#include "layer.h"

#include <QJsonObject>

#include <memory>

namespace fls {
struct Project;
}

namespace fls::scene {

void ensureProjectSceneRoot(fls::Project &project);

QJsonObject sceneTreeToJson(const Group &root);
std::unique_ptr<Group> sceneTreeFromJson(const QJsonObject &object);

} // namespace fls::scene
