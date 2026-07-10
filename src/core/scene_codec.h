#pragma once

#include "layer.h"

#include <QJsonObject>

#include <memory>

namespace fh6 {
struct Project;
}

namespace fh6::scene {

void ensureProjectSceneRoot(fh6::Project &project);

// Recursive tree serialization used by editor project format v2.
QJsonObject sceneTreeToJson(const Group &root);
std::unique_ptr<Group> sceneTreeFromJson(const QJsonObject &object);

} // namespace fh6::scene
