#include "layer.h"
#include "scene_codec.h"

#include <QtCore>

#include <memory>

int main()
{
    fh6::scene::Group root;
    root.id = QStringLiteral("__root__");
    auto guide = std::make_unique<fh6::scene::GuideLayer>();
    guide->id = QStringLiteral("guide-1");
    guide->preprocessColorCount = 11;
    root.append(std::move(guide));

    const QJsonObject encoded = fh6::scene::sceneTreeToJson(root);
    const std::unique_ptr<fh6::scene::Group> decoded = fh6::scene::sceneTreeFromJson(encoded);
    if (!decoded || decoded->children.size() != 1
        || decoded->children.front()->kind() != fh6::scene::LayerKind::Guide) {
        qCritical() << "guide metadata round trip did not preserve the guide";
        return 1;
    }
    const auto *decodedGuide = static_cast<const fh6::scene::GuideLayer *>(decoded->children.front().get());
    if (decodedGuide->preprocessColorCount != 11) {
        qCritical() << "retained color count was not serialized";
        return 1;
    }
    return 0;
}
