#include "image_io.h"

#include <QtCore>
#include <QtGui>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <wincodec.h>
#endif

namespace gui {
namespace {

QString supportedImageFormatsText()
{
    QStringList formats;
    for (const QByteArray &format : QImageReader::supportedImageFormats()) {
        formats.push_back(QString::fromLatin1(format));
    }
    formats.sort();
    return formats.join(QStringLiteral(", "));
}

#ifdef Q_OS_WIN
template <typename T>
void releaseCom(T *object)
{
    if (object != nullptr) {
        object->Release();
    }
}

QImage readImageWithWic(const QString &path, QString *error)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        if (error != nullptr) {
            *error = QStringLiteral("could not initialize Windows image decoder");
        }
        return {};
    }

    IWICImagingFactory *factory = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *converter = nullptr;
    QImage image;

    const std::wstring widePath = path.toStdWString();
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(widePath.c_str(),
                                                nullptr,
                                                GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad,
                                                &decoder);
    }
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame,
                                   GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) {
        hr = converter->GetSize(&width, &height);
    }
    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        image = QImage(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32_Premultiplied);
        hr = converter->CopyPixels(nullptr,
                                   static_cast<UINT>(image.bytesPerLine()),
                                   static_cast<UINT>(image.sizeInBytes()),
                                   image.bits());
        if (FAILED(hr)) {
            image = {};
        } else {
            image = image.mirrored(false, true);
        }
    }

    releaseCom(converter);
    releaseCom(frame);
    releaseCom(decoder);
    releaseCom(factory);
    if (uninitialize) {
        CoUninitialize();
    }
    if (image.isNull() && error != nullptr) {
        *error = QStringLiteral("Windows could not decode this image");
    }
    return image;
}
#endif

} // namespace

QStringList supportedImageSuffixes()
{
    QStringList suffixes;
    for (const QByteArray &format : QImageReader::supportedImageFormats()) {
        const QString suffix = QString::fromLatin1(format).toLower();
        if (!suffixes.contains(suffix)) {
            suffixes.push_back(suffix);
        }
        if (suffix == QStringLiteral("jpg") && !suffixes.contains(QStringLiteral("jpeg"))) {
            suffixes.push_back(QStringLiteral("jpeg"));
        }
    }
#ifdef Q_OS_WIN
    for (const QString &suffix : {
             QStringLiteral("bmp"),
             QStringLiteral("dib"),
             QStringLiteral("gif"),
             QStringLiteral("ico"),
             QStringLiteral("jfif"),
             QStringLiteral("jpe"),
             QStringLiteral("jpeg"),
             QStringLiteral("jpg"),
             QStringLiteral("jxr"),
             QStringLiteral("png"),
             QStringLiteral("tif"),
             QStringLiteral("tiff"),
             QStringLiteral("wdp"),
         }) {
        if (!suffixes.contains(suffix)) {
            suffixes.push_back(suffix);
        }
    }
#endif
    return suffixes;
}

QString imageDialogFilter()
{
    QStringList suffixes = supportedImageSuffixes();
    suffixes.sort();
    QStringList patterns;
    for (const QString &suffix : suffixes) {
        patterns.push_back(QStringLiteral("*.%1").arg(suffix));
    }
    return QStringLiteral("Images (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

QImage readGuideImage(const QString &path, QByteArray *format, QString *error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (!image.isNull()) {
        const QByteArray decodedFormat = reader.format();
        if (decodedFormat.compare(QByteArrayLiteral("webp"), Qt::CaseInsensitive) == 0) {
            image = image.mirrored(false, true);
        }
        if (format != nullptr) {
            *format = decodedFormat;
        }
        return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

#ifdef Q_OS_WIN
    image = readImageWithWic(path, error);
    if (!image.isNull()) {
        if (format != nullptr) {
            *format = QFileInfo(path).suffix().toLatin1().toLower();
        }
        return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
#endif

    if (error != nullptr && error->isEmpty()) {
        *error = QStringLiteral("Qt cannot decode this image: %1\nSupported Qt formats: %2")
                     .arg(path, supportedImageFormatsText());
    }
    return {};
}

QByteArray encodeGuideImage(const QImage &image, QString *formatOut)
{
    if (image.isNull()) {
        return {};
    }
    // Writers receive unpremultiplied ARGB32.
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);

    const auto encode = [&source](const char *format, int quality) -> QByteArray {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly)) {
            return {};
        }
        QImageWriter writer(&buffer, QByteArray(format));
        if (quality >= 0) {
            writer.setQuality(quality);
        }
        if (!writer.write(source)) {
            return {};
        }
        return bytes;
    };

    if (QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("webp"))) {
        const QByteArray webp = encode("webp", 100);
        if (!webp.isEmpty()) {
            if (formatOut != nullptr) {
                *formatOut = QStringLiteral("webp");
            }
            return webp;
        }
    }

    const QByteArray png = encode("png", -1);
    if (!png.isEmpty()) {
        if (formatOut != nullptr) {
            *formatOut = QStringLiteral("png");
        }
        return png;
    }
    return {};
}

} // namespace gui
