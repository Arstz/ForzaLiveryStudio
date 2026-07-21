#include "layer.h"

#include "matrix_math.h"

#include <cmath>
#include <utility>

namespace fh6::scene {

namespace {
constexpr double Pi = 3.14159265358979323846;
}

Matrix3 Transform2D::matrix() const {
    const double radians = rotation * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Matrix3 result = affine(1.0, 0.0, x, 0.0, 1.0, y);
    result = detail::multiply(result, affine(c, -s, 0.0, s, c, 0.0));
    result = detail::multiply(result, affine(1.0, skew, 0.0, 0.0, 1.0, 0.0));
    result = detail::multiply(result, affine(scaleX, 0.0, 0.0, 0.0, scaleY, 0.0));
    return result;
}

void Layer::copyBaseTo(Layer &dst) const {
    dst.id = id;
    dst.name = name;
    dst.transform = transform;
    dst.opacity = opacity;
    dst.visible = visible;
    dst.locked = locked;
}

Matrix3 Layer::worldMatrix() const {
    const Matrix3 local = transform.matrix();
    if (parent_ != nullptr) {
        return detail::multiply(parent_->worldMatrix(), local);
    }
    return local;
}

std::unique_ptr<Layer> Shape::clone() const {
    auto copy = std::make_unique<Shape>();
    copyBaseTo(*copy);
    copy->visual = visual ? visual->clone() : nullptr;
    copy->shapeId = shapeId;
    copy->raster = raster;
    copy->rasterId = rasterId;
    copy->rasterWidth = rasterWidth;
    copy->rasterHeight = rasterHeight;
    copy->color = color;
    copy->mask = mask;
    copy->sourceShape = sourceShape;
    copy->absOffset = absOffset;
    copy->marker = marker;
    copy->flags = flags;
    copy->sourceLogoId = sourceLogoId;
    copy->hasSourceTransform = hasSourceTransform;
    copy->sourceTransform = sourceTransform;
    return copy;
}

bool Shape::isRaster() const {
    return raster;
}

void Shape::setVectorShape(quint16 id) {
    shapeId = id;
    raster = false;
    rasterId = 0;
    auto vector = std::make_unique<VectorContainer>();
    vector->shapeId = id;
    visual = std::move(vector);
}

void Shape::setRasterShape(quint32 id, int width, int height) {
    raster = true;
    rasterId = id;
    rasterWidth = width > 0 ? width : 256;
    rasterHeight = height > 0 ? height : 256;
    auto raster = std::make_unique<RasterContainer>();
    raster->rasterId = id;
    raster->width = rasterWidth;
    raster->height = rasterHeight;
    visual = std::move(raster);
}

std::unique_ptr<Layer> GuideLayer::clone() const {
    auto copy = std::make_unique<GuideLayer>();
    copyBaseTo(*copy);
    copy->image = image ? std::make_unique<RasterContainer>(*image) : nullptr;
    copy->sourcePath = sourcePath;
    copy->preprocessColorCount = preprocessColorCount;
    return copy;
}

std::unique_ptr<Layer> Group::clone() const {
    auto copy = std::make_unique<Group>();
    copyBaseTo(*copy);
    copy->isLiverySection = isLiverySection;
    copy->liverySectionSlot = liverySectionSlot;
    copy->sourceAbsPos = sourceAbsPos;
    copy->pendingTransformMarker = pendingTransformMarker;
    copy->inlineTransformMarker = inlineTransformMarker;
    copy->effectiveTransformMarker = effectiveTransformMarker;
    copy->headerControlBytes = headerControlBytes;
    copy->flags = flags;
    copy->sourceParentId = sourceParentId;
    copy->sourcePreviousSiblingId = sourcePreviousSiblingId;
    copy->sourcePreviousGroupDepth = sourcePreviousGroupDepth;
    copy->sourceChildren = sourceChildren;
    copy->children.reserve(children.size());
    for (const auto &child : children) {
        copy->append(child->clone());
    }
    return copy;
}

Layer *Group::append(std::unique_ptr<Layer> child) {
    if (!child) {
        return nullptr;
    }
    child->parent_ = this;
    Layer *raw = child.get();
    children.push_back(std::move(child));
    return raw;
}

Layer *Group::insert(int index, std::unique_ptr<Layer> child) {
    if (!child) {
        return nullptr;
    }
    child->parent_ = this;
    Layer *raw = child.get();
    if (index < 0 || index >= static_cast<int>(children.size())) {
        children.push_back(std::move(child));
    } else {
        children.insert(children.begin() + index, std::move(child));
    }
    return raw;
}

std::unique_ptr<Layer> Group::takeAt(int index) {
    if (index < 0 || index >= static_cast<int>(children.size())) {
        return nullptr;
    }
    std::unique_ptr<Layer> child = std::move(children[index]);
    children.erase(children.begin() + index);
    if (child) {
        child->parent_ = nullptr;
    }
    return child;
}

} // namespace fh6::scene
