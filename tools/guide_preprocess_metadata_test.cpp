#include "layer.h"
#include "scene_codec.h"

#include <QtCore>

#include <memory>

int main()
{
    fls::scene::Group root;
    root.id = QStringLiteral("__root__");
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = QStringLiteral("guide-1");
    guide->preprocessColorCount = 11;
    root.append(std::move(guide));

    const QJsonObject encoded = fls::scene::sceneTreeToJson(root);
    const std::unique_ptr<fls::scene::Group> decoded = fls::scene::sceneTreeFromJson(encoded);
    if (!decoded || decoded->children.size() != 1
        || decoded->children.front()->kind() != fls::scene::LayerKind::Guide) {
        qCritical() << "guide metadata round trip did not preserve the guide";
        return 1;
    }
    const auto *decodedGuide = static_cast<const fls::scene::GuideLayer *>(decoded->children.front().get());
    if (decodedGuide->preprocessColorCount != 11) {
        qCritical() << "retained color count was not serialized";
        return 1;
    }
    return 0;
}
