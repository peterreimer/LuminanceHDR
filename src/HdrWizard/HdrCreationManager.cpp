/**
 * This file is a part of Luminance HDR package
 * ---------------------------------------------------------------------- 
 * Copyright (C) 2006,2007 Giuseppe Rota
 * Copyright (C) 2010-2012 Franco Comida
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * ---------------------------------------------------------------------- 
 *
 * @author Giuseppe Rota <grota@users.sourceforge.net>
 *
 * Manual and auto antighosting, improvements, bugfixing
 * @author Franco Comida <fcomida@users.sourceforge.net>
 *
 */

#include "HdrCreationManager.h"

#include <QDebug>
#include <QApplication>
#include <QFileInfo>
#include <QFile>
#include <QColor>
#include <QScopedPointer>
#include <QtConcurrentMap>
#include <QtConcurrentFilter>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#include <boost/bind.hpp>
#include <boost/numeric/conversion/bounds.hpp>
#include <boost/limits.hpp>

#include "Common/CommonFunctions.h"
#include <Libpfs/frame.h>
#include <Libpfs/utils/msec_timer.h>
#include <Libpfs/io/tiffwriter.h>
#include <Libpfs/io/tiffreader.h>
#include <Libpfs/io/framereader.h>
#include <Libpfs/io/framereaderfactory.h>
#include <Libpfs/io/framewriter.h>
#include <Libpfs/io/framewriterfactory.h>
#include <Libpfs/utils/transform.h>
#include <Libpfs/manip/shift.h>
#include <Libpfs/manip/copy.h>
#include <Libpfs/manip/cut.h>
#include <Libpfs/colorspace/convert.h>
#include <Libpfs/colorspace/colorspace.h>

#include "arch/math.h"
#include "TonemappingOperators/fattal02/pde.h"
#include "Exif/ExifOperations.h"
#include "HdrCreation/mtb_alignment.h"
#include "WhiteBalance.h"

using namespace std;
using namespace pfs;
using namespace pfs::io;
using namespace libhdr::fusion;

const FusionOperatorConfig predef_confs[6] =
{
    {WEIGHT_TRIANGULAR, RESPONSE_LINEAR, DEBEVEC, QString(), QString()},
    {WEIGHT_TRIANGULAR, RESPONSE_GAMMA, DEBEVEC, QString(), QString()},
    {WEIGHT_PLATEAU, RESPONSE_LINEAR, DEBEVEC, QString(), QString()},
    {WEIGHT_PLATEAU, RESPONSE_GAMMA, DEBEVEC, QString(), QString()},
    {WEIGHT_GAUSSIAN, RESPONSE_LINEAR, DEBEVEC, QString(), QString()},
    {WEIGHT_GAUSSIAN, RESPONSE_GAMMA, DEBEVEC, QString(), QString()},
};

// --- NEW CODE ---
namespace
{

QImage* shiftQImage(const QImage *in, int dx, int dy)
{
    QImage *out = new QImage(in->size(),QImage::Format_ARGB32);
    assert(out!=NULL);
    out->fill(qRgba(0,0,0,0)); //transparent black
    for(int i = 0; i < in->height(); i++)
    {
        if( (i+dy) < 0 ) continue;
        if( (i+dy) >= in->height()) break;
        QRgb *inp = (QRgb*)in->scanLine(i);
        QRgb *outp = (QRgb*)out->scanLine(i+dy);
        for(int j = 0; j < in->width(); j++)
        {
            if( (j+dx) >= in->width()) break;
            if( (j+dx) >= 0 ) outp[j+dx] = *inp;
            inp++;
        }
    }
    return out;
}

void shiftItem(HdrCreationItem& item, int dx, int dy)
{
    FramePtr shiftedFrame( pfs::shift(*item.frame(), dx, dy) );
    item.frame().swap(shiftedFrame);
    shiftedFrame.reset();       // release memory

    QScopedPointer<QImage> img(shiftQImage(&item.qimage(), dx, dy));
    item.qimage().swap( *img );
    img.reset();    // release memory
}
}

static
bool checkFileName(const HdrCreationItem& item, const QString& str) {
    return (item.filename().compare(str) == 0);
}
  
void HdrCreationManager::loadFiles(const QStringList &filenames)
{
    for(const auto filename : filenames)
    {
        qDebug() << QString("HdrCreationManager::loadFiles(): Checking %1").arg(filename);
        HdrCreationItemContainer::iterator it = find_if(m_data.begin(), m_data.end(),
                                                        boost::bind(&checkFileName, _1, filename));
        // has the file been inserted already?
        if (it == m_data.end())
        {
            qDebug() << QString("HdrCreationManager::loadFiles(): Schedule loading for %1").arg(filename);
            m_tmpdata.push_back(HdrCreationItem(filename));
        }
        else
        {
            qDebug() << QString("HdrCreationManager::loadFiles(): %1 has already been loaded").arg(filename);
        }
    }

    // parallel load of the data...
    connect(&m_futureWatcher, SIGNAL(finished()), this, SLOT(loadFilesDone()), Qt::DirectConnection);

    // Start the computation.
    m_futureWatcher.setFuture( QtConcurrent::map(m_tmpdata.begin(), m_tmpdata.end(), LoadFile()) );
}

void HdrCreationManager::loadFilesDone()
{ 
    qDebug() << "HdrCreationManager::loadFilesDone(): Data loaded ... move to internal structure!";
    if (m_futureWatcher.isCanceled() ) // LoadFile() threw an exception
    {
        emit errorWhileLoading(tr("HdrCreationManager::loadFilesDone(): Error loading a file."));
        disconnect(&m_futureWatcher, SIGNAL(finished()), this, SLOT(loadFilesDone()));
        m_tmpdata.clear();
        return;
    }

    if (isLoadResponseCurve())
    {
        try
        {
            const int bps = m_tmpdata[0].getBitDepth();
            m_response->setBPS(bps);
            m_weight->setBPS(bps);
            m_response->readFromFile(
                    QFile::encodeName(getResponseCurveInputFilename()).constData());
            setLoadResponseCurve(false);
        }
        catch(std::runtime_error &e)
        {
            emit errorWhileLoading(QString(e.what()));
        }
    }
    disconnect(&m_futureWatcher, SIGNAL(finished()), this, SLOT(loadFilesDone()));
    for(const auto hdrCreationItem : m_tmpdata)
    {
        if (hdrCreationItem.isValid())
        {
            qDebug() << QString("HdrCreationManager::loadFilesDone(): Insert data for %1").arg(hdrCreationItem.filename());
            m_data.push_back(hdrCreationItem);
        }
    }
    m_tmpdata.clear();

    refreshEVOffset();

    if (!framesHaveSameSize())
    {
        m_data.clear();
        emit errorWhileLoading(tr("HdrCreationManager::loadFilesDone(): The images have different size."));
    }
    else
    {
        emit finishedLoadingFiles();
    }
}

void HdrCreationManager::refreshEVOffset()
{
    // no data
    if (m_data.size() <= 0)
    {
        m_evOffset = 0.f;
        return;
    }

    std::vector<float> evs;
    for(const auto hdrCreationItem : m_data)
    {
        if (hdrCreationItem.hasEV())
        {
            evs.push_back(hdrCreationItem.getEV());
        }
    }

    // no image has EV
    if (evs.size() <= 0)
    {
        m_evOffset = 0.f;
        return;
    }

    // only one image available
    if (evs.size() == 1)
    {
        m_evOffset = evs[0];
        return;
    }

    // sort...
    std::sort(evs.begin(), evs.end());
    m_evOffset = evs[(evs.size() + 1)/2 - 1];

    qDebug() << QString("HdrCreationManager::refreshEVOffset(): offset = %1").arg(m_evOffset);
}

float HdrCreationManager::getEVOffset() const
{
    return m_evOffset;
}

QStringList HdrCreationManager::getFilesWithoutExif() const
{
    QStringList invalidFiles;
    foreach (const HdrCreationItem& fileData, m_data) {
        if ( !fileData.hasAverageLuminance() ) {
            invalidFiles.push_back( fileData.filename() );
        }
    }
    return invalidFiles;
}

size_t HdrCreationManager::numFilesWithoutExif() const {
    size_t counter = 0;
    foreach (const HdrCreationItem& fileData, m_data) {
        if ( !fileData.hasAverageLuminance() ) {
            ++counter;
        }
    }
    return counter;
}

void HdrCreationManager::removeFile(int idx)
{
    Q_ASSERT(idx >= 0);
    Q_ASSERT(idx < (int)m_data.size());

    m_data.erase(m_data.begin() + idx);

    refreshEVOffset();
}

HdrCreationManager::HdrCreationManager(bool fromCommandLine)
    : m_evOffset(0.f)
    , m_response(new ResponseCurve(predef_confs[0].responseCurve))
    , m_weight(new WeightFunction(predef_confs[0].weightFunction))
    , m_responseCurveInputFilename()
    , m_agMask(NULL)
    , m_align()
    , m_ais_crop_flag(false)
    , fromCommandLine(fromCommandLine)
    , m_isLoadResponseCurve(false)
{
    // setConfig(predef_confs[0]);
    setFusionOperator(predef_confs[0].fusionOperator);

    for (int i = 0; i < agGridSize; i++)
    {
        for (int j = 0; j < agGridSize; j++)
        {
            m_patches[i][j] = false;
        }
    }

    connect(&m_futureWatcher, SIGNAL(started()), this, SIGNAL(progressStarted()), Qt::DirectConnection);
    connect(&m_futureWatcher, SIGNAL(finished()), this, SIGNAL(progressFinished()), Qt::DirectConnection);
    connect(this, SIGNAL(progressCancel()), &m_futureWatcher, SLOT(cancel()), Qt::DirectConnection);
    connect(&m_futureWatcher, SIGNAL(progressRangeChanged(int,int)), this, SIGNAL(progressRangeChanged(int,int)), Qt::DirectConnection);
    connect(&m_futureWatcher, SIGNAL(progressValueChanged(int)), this, SIGNAL(progressValueChanged(int)), Qt::DirectConnection);
}

void HdrCreationManager::setConfig(const FusionOperatorConfig &c)
{
    if (!c.inputResponseCurveFilename.isEmpty())
    {
        setLoadResponseCurve(true);
        setResponseCurveInputFilename(c.inputResponseCurveFilename);
    }
    else
    {
        m_response->setType(c.responseCurve);
    }
    getWeightFunction().setType(c.weightFunction);
    setFusionOperator(c.fusionOperator);
}

QVector<float> HdrCreationManager::getExpotimes() const
{
    QVector<float> expotimes;
    for ( HdrCreationItemContainer::const_iterator it = m_data.begin(), 
          itEnd = m_data.end(); it != itEnd; ++it) {
        expotimes.push_back(it->getEV());
    }
    return expotimes;
}

bool HdrCreationManager::framesHaveSameSize()
{
    size_t width = m_data[0].frame()->getWidth();
    size_t height = m_data[0].frame()->getHeight();
    for ( HdrCreationItemContainer::const_iterator it = m_data.begin() + 1, 
          itEnd = m_data.end(); it != itEnd; ++it) {
        if (it->frame()->getWidth() != width || it->frame()->getHeight() != height)
            return false; 
    }
    return true;
}

void HdrCreationManager::align_with_mtb()
{
    // build temporary container...
    vector<FramePtr> frames;
    for (size_t i = 0; i < m_data.size(); ++i) {
        frames.push_back( m_data[i].frame() );
    }

    // run MTB
    libhdr::mtb_alignment(frames);

    // rebuild previews
    QFutureWatcher<void> futureWatcher;
    futureWatcher.setFuture( QtConcurrent::map(m_data.begin(), m_data.end(), RefreshPreview()) );
    futureWatcher.waitForFinished();

    // emit finished
    emit finishedAligning(0);
}

void HdrCreationManager::set_ais_crop_flag(bool flag)
{
    m_ais_crop_flag = flag;
}

void HdrCreationManager::align_with_ais()
{
    m_align.reset(new Align(m_data, fromCommandLine, 1));
    connect(m_align.get(), SIGNAL(finishedAligning(int)), this, SIGNAL(finishedAligning(int)));
    connect(m_align.get(), SIGNAL(failedAligning(QProcess::ProcessError)), this, SIGNAL(ais_failed(QProcess::ProcessError)));
    connect(m_align.get(), SIGNAL(failedAligning(QProcess::ProcessError)), this, SLOT(ais_failed_slot(QProcess::ProcessError)));
    connect(m_align.get(), SIGNAL(dataReady(QByteArray)), this, SIGNAL(aisDataReady(QByteArray)));
  
    m_align->align_with_ais(m_ais_crop_flag);
}

void HdrCreationManager::ais_failed_slot(QProcess::ProcessError error)
{
    qDebug() << "align_image_stack failed";
}

void HdrCreationManager::removeTempFiles()
{
    if (m_align)
    {
        m_align->removeTempFiles();
    }
}

pfs::Frame* HdrCreationManager::createHdr()
{
    const int bps = m_data[0].getBitDepth();

    std::vector<FrameEnhanced> frames;

    for (size_t idx = 0; idx < m_data.size(); ++idx)
    {
        frames.push_back(
                    FrameEnhanced(
                        m_data[idx].frame(),
                        std::pow(2.f, m_data[idx].getEV() - m_evOffset),
                        bps
                        )
                    );
    }

    libhdr::fusion::FusionOperatorPtr fusionOperatorPtr = IFusionOperator::build(m_fusionOperator);
    pfs::Frame* outputFrame(fusionOperatorPtr->computeFusion(*m_response, *m_weight, frames));

    if (!m_responseCurveOutputFilename.isEmpty())
    {
        m_response->writeToFile(QFile::encodeName(m_responseCurveOutputFilename).constData());
    }

    return outputFrame;
}

void HdrCreationManager::applyShiftsToItems(const QList<QPair<int,int> >& hvOffsets)
{
    int size = m_data.size();
    //shift the frames and images
    for (int i = 0; i < size; i++)
    {
        if ( hvOffsets[i].first == hvOffsets[i].second &&
             hvOffsets[i].first == 0 )
        {
            continue;
        }
        shiftItem(m_data[i],
                  hvOffsets[i].first,
                  hvOffsets[i].second);
    }
}

void HdrCreationManager::cropItems(const QRect& ca)
{
    // crop all frames and images
    int size = m_data.size();
    for (int idx = 0; idx < size; idx++)
    {
        std::unique_ptr<QImage> newimage(new QImage(m_data[idx].qimage().copy(ca)));
        if (newimage == NULL)
        {
            exit(1); // TODO: exit gracefully
        }
        m_data[idx].qimage().swap(*newimage);
        newimage.reset();

        int x_ul, y_ur, x_bl, y_br;
        ca.getCoords(&x_ul, &y_ur, &x_bl, &y_br);

        FramePtr cropped(
                    cut(m_data[idx].frame().get(),
                        static_cast<size_t>(x_ul), static_cast<size_t>(y_ur),
                        static_cast<size_t>(x_bl), static_cast<size_t>(y_br))
                    );
        m_data[idx].frame().swap(cropped);
        cropped.reset();
    }
}

HdrCreationManager::~HdrCreationManager()
{
    this->reset();
    delete m_agMask;
}

void HdrCreationManager::saveImages(const QString& prefix)
{
    int idx = 0;
    for ( HdrCreationItemContainer::const_iterator it = m_data.begin(), 
          itEnd = m_data.end(); it != itEnd; ++it) {

        QString filename = prefix + QString("_%1").arg(idx) + ".tiff";
        pfs::io::TiffWriter writer(QFile::encodeName(filename).constData());
        writer.write( *it->frame(), pfs::Params("tiff_mode", 1) );

        QFileInfo qfi(filename);
        QString absoluteFileName = qfi.absoluteFilePath();
        QByteArray encodedName = QFile::encodeName(absoluteFileName);
        ExifOperations::copyExifData(QFile::encodeName(it->filename()).constData(), encodedName.constData(), false);
        ++idx;
    }
    emit imagesSaved();
}

int HdrCreationManager::computePatches(float threshold, bool patches[][agGridSize], float &percent, QList <QPair<int, int> > HV_offset)
{
    qDebug() << "HdrCreationManager::computePatches";
    qDebug() << threshold;
#ifdef TIMER_PROFILING
    msec_timer stop_watch;
    stop_watch.start();
#endif
    const int width = m_data[0].frame()->getWidth();
    const int height = m_data[0].frame()->getHeight();
    const int gridX = width / agGridSize;
    const int gridY = height / agGridSize;
    const int size = m_data.size(); 
    assert(size >= 2);

    vector<float> HE(size);

    hueSquaredMean(m_data, HE);

    m_agGoodImageIndex = findIndex(HE.data(), size);
    qDebug() << "h0: " << m_agGoodImageIndex;

    for (int j = 0; j < agGridSize; j++) {
        for (int i = 0; i < agGridSize; i++) {
            m_patches[i][j] = false;
        }
    }

    for (int h = 0; h < size; h++) {
        if (h == m_agGoodImageIndex) 
            continue;
        float deltaEV = log2(m_data[m_agGoodImageIndex].getAverageLuminance()) - log2(m_data[h].getAverageLuminance());
        int dx = HV_offset[m_agGoodImageIndex].first - HV_offset[h].first;
        int dy = HV_offset[m_agGoodImageIndex].second - HV_offset[h].second;        
        float sR, sG, sB;
        sdv(m_data[m_agGoodImageIndex], m_data[h], deltaEV, dx, dy, sR, sG, sB); 
        //#pragma omp parallel for schedule(static)
        for (int j = 0; j < agGridSize; j++) {
            for (int i = 0; i < agGridSize; i++) {
                if (comparePatches(m_data[m_agGoodImageIndex],
                                   m_data[h],
                                   i, j, gridX, gridY, threshold, sR, sG, sB, deltaEV, dx, dy)) {
                    m_patches[i][j] = true;
                }
            }                      
        }
    }

    int count = 0;
    for (int i = 0; i < agGridSize; i++)
        for (int j = 0; j < agGridSize; j++)
            if (m_patches[i][j] == true)
                count++;
    percent = static_cast<float>(count) / static_cast<float>(agGridSize*agGridSize) * 100.0f;
    qDebug() << "Total patches: " << percent << "%";

    memcpy(patches, m_patches, agGridSize*agGridSize);

#ifdef TIMER_PROFILING
    stop_watch.stop_and_update();
    std::cout << "computePatches = " << stop_watch.get_time() << " msec" << std::endl;
#endif
    return m_agGoodImageIndex;
}

pfs::Frame *HdrCreationManager::doAntiGhosting(bool patches[][agGridSize], int h0, bool manualAg, ProgressHelper *ph)
{
    qDebug() << "HdrCreationManager::doAntiGhosting";
#ifdef TIMER_PROFILING
    msec_timer stop_watch;
    stop_watch.start();
#endif
    const int width = m_data[0].frame()->getWidth();
    const int height = m_data[0].frame()->getHeight();
    const int gridX = width / agGridSize;
    const int gridY = height / agGridSize;
    connect(ph, SIGNAL(qtSetRange(int, int)), this, SIGNAL(progressRangeChanged(int, int)));
    connect(ph, SIGNAL(qtSetValue(int)), this, SIGNAL(progressValueChanged(int)));
    ph->setRange(0,100);
    ph->setValue(0);
    emit progressStarted();

    const Channel *Good_Xc, *Good_Yc, *Good_Zc;
    m_data[h0].frame().get()->getXYZChannels(Good_Xc, Good_Yc, Good_Zc);

    const Channel *Xc, *Yc, *Zc;
    Frame* ghosted = createHdr();
    ghosted->getXYZChannels(Xc, Yc, Zc);
    ph->setValue(20);
    if (ph->canceled()) return NULL;

    Array2Df Good_Rc(*Good_Xc);
    Array2Df Good_Gc(*Good_Yc);
    Array2Df Good_Bc(*Good_Zc);

    Array2Df Rc(*Xc);
    Array2Df Gc(*Xc);
    Array2Df Bc(*Xc);

    delete ghosted;

    this->reset();

    //RED
    Array2Df logIrradiance_R(width, height);
    computeLogIrradiance(logIrradiance_R, Rc);
    Rc.reset();

    Array2Df gradientXGood_R(width, height);
    Array2Df gradientYGood_R(width, height);
    Array2Df logIrradianceGood_R(width, height);
    computeLogIrradiance(logIrradianceGood_R, Good_Rc);
    computeGradient(gradientXGood_R, gradientYGood_R, logIrradianceGood_R);
    Good_Rc.reset();
    logIrradianceGood_R.reset();

    Array2Df gradientX_R(width, height);
    Array2Df gradientY_R(width, height);
    computeGradient(gradientX_R, gradientY_R, logIrradiance_R);

    Array2Df gradientXBlended_R(width, height);
    Array2Df gradientYBlended_R(width, height);

    if (manualAg)
        blendGradients(gradientXBlended_R, gradientYBlended_R,
                       gradientX_R, gradientY_R,
                       gradientXGood_R, gradientYGood_R,
                       *m_agMask);
    else
        blendGradients(gradientXBlended_R, gradientYBlended_R,
                       gradientX_R, gradientY_R,
                       gradientXGood_R, gradientYGood_R,
                       patches, gridX, gridY);
    gradientX_R.reset();
    gradientY_R.reset();
    gradientXGood_R.reset();
    gradientYGood_R.reset();

    Array2Df divergence_R(width, height);
    computeDivergence(divergence_R, gradientXBlended_R, gradientYBlended_R);
    gradientXBlended_R.reset();
    gradientYBlended_R.reset();
    //END RED

    //GREEN
    Array2Df logIrradiance_G(width, height);
    computeLogIrradiance(logIrradiance_G, Gc);
    Gc.reset();

    Array2Df gradientXGood_G(width, height);
    Array2Df gradientYGood_G(width, height);
    Array2Df logIrradianceGood_G(width, height);
    computeGradient(gradientXGood_G, gradientYGood_G, logIrradianceGood_G);

    computeLogIrradiance(logIrradianceGood_G, Good_Gc);
    Good_Gc.reset();
    logIrradianceGood_G.reset();

    Array2Df gradientX_G(width, height);
    Array2Df gradientY_G(width, height);
    computeGradient(gradientX_G, gradientY_G, logIrradiance_G);

    Array2Df gradientXBlended_G(width, height);
    Array2Df gradientYBlended_G(width, height);

    if (manualAg)
        blendGradients(gradientXBlended_G, gradientYBlended_G,
                       gradientX_G, gradientY_G,
                       gradientXGood_G, gradientYGood_G,
                       *m_agMask);
    else
        blendGradients(gradientXBlended_G, gradientYBlended_G,
                       gradientX_G, gradientY_G,
                       gradientXGood_G, gradientYGood_G,
                       patches, gridX, gridY);
    gradientX_G.reset();
    gradientY_G.reset();
    gradientXGood_G.reset();
    gradientYGood_G.reset();

    Array2Df divergence_G(width, height);
    computeDivergence(divergence_G, gradientXBlended_G, gradientYBlended_G);
    gradientXBlended_G.reset();
    gradientYBlended_G.reset();
    //END GREEN

    //BLUE
    Array2Df logIrradiance_B(width, height);
    computeLogIrradiance(logIrradiance_B, Bc);
    Bc.reset();

    Array2Df gradientXGood_B(width, height);
    Array2Df gradientYGood_B(width, height);
    Array2Df logIrradianceGood_B(width, height);
    computeLogIrradiance(logIrradianceGood_B, Good_Bc);
    computeGradient(gradientXGood_B, gradientYGood_B, logIrradianceGood_B);
    Good_Bc.reset();
    logIrradianceGood_B.reset();

    Array2Df gradientX_B(width, height);
    Array2Df gradientY_B(width, height);
    computeGradient(gradientX_B, gradientY_B, logIrradiance_B);

    Array2Df gradientXBlended_B(width, height);
    Array2Df gradientYBlended_B(width, height);

    if (manualAg)
        blendGradients(gradientXBlended_B, gradientYBlended_B,
                       gradientX_B, gradientY_B,
                       gradientXGood_B, gradientYGood_B,
                       *m_agMask);
    else
        blendGradients(gradientXBlended_B, gradientYBlended_B,
                       gradientX_B, gradientY_B,
                       gradientXGood_B, gradientYGood_B,
                       patches, gridX, gridY);
    gradientX_B.reset();
    gradientY_B.reset();
    gradientXGood_B.reset();
    gradientYGood_B.reset();

    Array2Df divergence_B(width, height);
    computeDivergence(divergence_B, gradientXBlended_B, gradientYBlended_B);
    gradientXBlended_G.reset();
    gradientYBlended_G.reset();
    //END BLUE

    qDebug() << "solve_pde";
    solve_pde_dct(divergence_R, logIrradiance_R);
    ph->setValue(60);
    if (ph->canceled()) { 
        return NULL;
    }

    qDebug() << "solve_pde";
    solve_pde_dct(divergence_G, logIrradiance_G);
    ph->setValue(76);
    if (ph->canceled()) { 
        return NULL;
    }

    qDebug() << "solve_pde";
    solve_pde_dct(divergence_B, logIrradiance_B);
    ph->setValue(93);
    if (ph->canceled()) { 
        return NULL;
    }

    QScopedPointer<Frame> deghosted(new Frame(width, height));
    Channel *Urc, *Ugc, *Ubc;
    deghosted->createXYZChannels(Urc, Ugc, Ubc);

    computeIrradiance(*Urc, logIrradiance_R);
    logIrradiance_R.reset();
    ph->setValue(94);
    if (ph->canceled()) { 
        return NULL;
    }
    computeIrradiance(*Ugc, logIrradiance_G);
    logIrradiance_G.reset();
    ph->setValue(95);
    if (ph->canceled()) { 
        return NULL;
    }
    computeIrradiance(*Ubc, logIrradiance_B);
    logIrradiance_B.reset();
    ph->setValue(96);
    if (ph->canceled()) { 
        return NULL;
    }

    float mr = min(*Urc);
    float mg = min(*Ugc);
    float mb = min(*Ubc);
    float t = min(mr, mg);
    float m = min(t,mb);

    clampToZero(*Urc, *Ugc, *Ubc, m);

    shadesOfGrayAWB(*Urc, *Ugc, *Ubc);

    ph->setValue(100);

    emit progressFinished();
#ifdef TIMER_PROFILING
    stop_watch.stop_and_update();
    std::cout << "doAntiGhosting = " << stop_watch.get_time() << " msec" << std::endl;
#endif
    return deghosted.take();
}

void HdrCreationManager::getAgData(bool patches[][agGridSize], int &h0)
{
    memcpy(patches, m_patches, agGridSize*agGridSize);

    h0 = m_agGoodImageIndex;
}

void HdrCreationManager::setPatches(bool patches[][agGridSize])
{
    memcpy(m_patches, patches, agGridSize*agGridSize);
}

void HdrCreationManager::reset()
{
    if (m_align != NULL) {
        m_align->reset();
        m_align->removeTempFiles();
    }

    if (m_futureWatcher.isRunning())
    {
        qDebug() << "Aborting loadFiles...";
        m_futureWatcher.cancel();
        m_futureWatcher.waitForFinished();
        emit loadFilesAborted();
    }

    disconnect(&m_futureWatcher, SIGNAL(finished()), this, SLOT(loadFilesDone()));
    m_data.clear();
    m_tmpdata.clear();
}

