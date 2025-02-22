#include "WaterfallCanvas.h"

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#if !wxUSE_GLCANVAS
#error "OpenGL required: set wxUSE_GLCANVAS to 1 and rebuild the library"
#endif

#include "CubicSDR.h"
#include "CubicSDRDefs.h"
#include "AppFrame.h"
#include <algorithm>

#ifdef USE_HAMLIB
#include "RigThread.h"
#endif

#include <wx/numformatter.h>

wxBEGIN_EVENT_TABLE(WaterfallCanvas, wxGLCanvas)
EVT_PAINT(WaterfallCanvas::OnPaint)
EVT_IDLE(WaterfallCanvas::OnIdle)
EVT_MOTION(WaterfallCanvas::OnMouseMoved)
EVT_LEFT_DOWN(WaterfallCanvas::OnMouseDown)
EVT_LEFT_UP(WaterfallCanvas::OnMouseReleased)
EVT_RIGHT_DOWN(WaterfallCanvas::OnMouseRightDown)
EVT_RIGHT_UP(WaterfallCanvas::OnMouseRightReleased)
EVT_LEAVE_WINDOW(WaterfallCanvas::OnMouseLeftWindow)
EVT_ENTER_WINDOW(WaterfallCanvas::OnMouseEnterWindow)
EVT_MOUSEWHEEL(WaterfallCanvas::OnMouseWheelMoved)
wxEND_EVENT_TABLE()

WaterfallCanvas::WaterfallCanvas(wxWindow *parent, int *attribList) :
        InteractiveCanvas(parent, attribList), dragState(WF_DRAG_NONE), nextDragState(WF_DRAG_NONE), fft_size(0), new_fft_size(0), waterfall_lines(0),
        dragOfs(0), mouseZoom(1), zoom(1), freqMoving(false), freqMove(0.0), hoverAlpha(1.0) {

    glContext = new PrimaryGLContext(this, &wxGetApp().GetContext(this));
    linesPerSecond = 30;
    lpsIndex = 0;
    preBuf = false;
    SetCursor(wxCURSOR_CROSS);
    scaleMove = 0;
    minBandwidth = 30000;
    fft_size_changed.store(false);
}

WaterfallCanvas::~WaterfallCanvas() {
}

void WaterfallCanvas::setup(unsigned int fft_size_in, int waterfall_lines_in) {
    if (fft_size == fft_size_in && waterfall_lines_in == waterfall_lines) {
        return;
    }
    fft_size = fft_size_in;
    waterfall_lines = waterfall_lines_in;

    waterfallPanel.setup(fft_size, waterfall_lines);
    gTimer.start();
}

void WaterfallCanvas::setFFTSize(unsigned int fft_size_in) {
    if (fft_size_in == fft_size) {
        return;
    }
    new_fft_size = fft_size_in;
    fft_size_changed.store(true);
}

WaterfallCanvas::DragState WaterfallCanvas::getDragState() {
    return dragState;
}

WaterfallCanvas::DragState WaterfallCanvas::getNextDragState() {
    return nextDragState;
}

void WaterfallCanvas::attachSpectrumCanvas(SpectrumCanvas *canvas_in) {
    spectrumCanvas = canvas_in;
}

void WaterfallCanvas::processInputQueue() {
    tex_update.lock();
    
    gTimer.update();
    
    double targetVis =  1.0 / (double)linesPerSecond;
    lpsIndex += gTimer.lastUpdateSeconds();

    bool updated = false;
    if (linesPerSecond) {
        if (lpsIndex >= targetVis) {
            while (lpsIndex >= targetVis) {
                SpectrumVisualData *vData;
                if (!visualDataQueue.empty()) {
                    visualDataQueue.pop(vData);
                    
                    if (vData) {
                        if (vData->spectrum_points.size() == fft_size * 2) {
                            waterfallPanel.setPoints(vData->spectrum_points);
                        }
                        waterfallPanel.step();
                        vData->decRefCount();
                        updated = true;
                    }
                    lpsIndex-=targetVis;
                } else {
                	break;
                }
            }
        }
    }
    if (updated) {
        wxClientDC(this);
        glContext->SetCurrent(*this);
        waterfallPanel.update();
    }
    tex_update.unlock();
}

void WaterfallCanvas::OnPaint(wxPaintEvent& WXUNUSED(event)) {
    tex_update.lock();
    wxPaintDC dc(this);
    
    const wxSize ClientSize = GetClientSize();
    long double currentZoom = zoom;
    
    if (mouseZoom != 1) {
        currentZoom = mouseZoom;
        mouseZoom = mouseZoom + (1.0 - mouseZoom) * 0.2;
        if (fabs(mouseZoom-1.0)<0.01) {
            mouseZoom = 1;
        }
    }
    
    if (scaleMove != 0) {
        SpectrumVisualProcessor *sp = wxGetApp().getSpectrumProcessor();
        FFTVisualDataThread *wdt = wxGetApp().getAppFrame()->getWaterfallDataThread();
        SpectrumVisualProcessor *wp = wdt->getProcessor();
        float factor = sp->getScaleFactor();

        factor += scaleMove * 0.02;
        
        if (factor < 0.25) {
            factor = 0.25;
        }
        if (factor > 10.0) {
            factor = 10.0;
        }
        
        sp->setScaleFactor(factor);
        wp->setScaleFactor(factor);
    }
    
    if (freqMove != 0.0) {
        long long newFreq = getCenterFrequency() + (long long)((long double)getBandwidth()*freqMove) * 0.01;
        
        long long minFreq = bandwidth/2;
        if (newFreq < minFreq) {
            newFreq = minFreq;
        }

        updateCenterFrequency(newFreq);
        
        if (!freqMoving) {
            freqMove -= (freqMove * 0.2);
            if (fabs(freqMove) < 0.01) {
                freqMove = 0.0;
            }
        }
    }
    
    long long bw;
    if (currentZoom != 1) {
        long long freq = wxGetApp().getFrequency();
        bw = getBandwidth();

        double mpos = 0;
        float mouseInView = false;

        if (mouseTracker.mouseInView()) {
            mpos = mouseTracker.getMouseX();
            mouseInView = true;
        } else if (spectrumCanvas && spectrumCanvas->getMouseTracker()->mouseInView()) {
            mpos = spectrumCanvas->getMouseTracker()->getMouseX();
            mouseInView = true;
        }

        if (currentZoom < 1) {
            bw = (long long) ceil((long double) bw * currentZoom);
            if (bw < minBandwidth) {
                bw = minBandwidth;
            }
            if (mouseInView) {
                long long mfreqA = getFrequencyAt(mpos, centerFreq, getBandwidth());
                long long mfreqB = getFrequencyAt(mpos, centerFreq, bw);
                centerFreq += mfreqA - mfreqB;
            }
            
            setView(centerFreq, bw);
        } else {
            if (isView) {
                bw = (long long) ceil((long double) bw * currentZoom);

                if (bw >= wxGetApp().getSampleRate()) {
                    disableView();
                    if (spectrumCanvas) {
                        spectrumCanvas->disableView();
                    }
                    bw = wxGetApp().getSampleRate();
                    centerFreq = wxGetApp().getFrequency();
                } else {
                    if (mouseInView) {
                        long long mfreqA = getFrequencyAt(mpos, centerFreq, getBandwidth());
                        long long mfreqB = getFrequencyAt(mpos, centerFreq, bw);
                        centerFreq += mfreqA - mfreqB;
                        setBandwidth(bw);
                    } else {
                        setBandwidth(bw);
                    }
                }
            }
        }
        if (centerFreq < freq && (centerFreq - bandwidth / 2) < (freq - wxGetApp().getSampleRate() / 2)) {
            centerFreq = (freq - wxGetApp().getSampleRate() / 2) + bandwidth / 2;
        }
        if (centerFreq > freq && (centerFreq + bandwidth / 2) > (freq + wxGetApp().getSampleRate() / 2)) {
            centerFreq = (freq + wxGetApp().getSampleRate() / 2) - bandwidth / 2;
        }

        if (spectrumCanvas) {
            if ((spectrumCanvas->getCenterFrequency() != centerFreq) || (spectrumCanvas->getBandwidth() != bw)) {
                if (getViewState()) {
                    spectrumCanvas->setView(centerFreq,bw);
                } else {
                    spectrumCanvas->disableView();
                    spectrumCanvas->setCenterFrequency(centerFreq);
                    spectrumCanvas->setBandwidth(bw);
                }
            }
        }
    }
    

    glContext->SetCurrent(*this);
    initGLExtensions();
    glViewport(0, 0, ClientSize.x, ClientSize.y);
    
    if (fft_size_changed.load()) {
        fft_size = new_fft_size;
        waterfallPanel.setup(fft_size, waterfall_lines);
        fft_size_changed.store(false);
    }

    glContext->BeginDraw(0,0,0);

    waterfallPanel.calcTransform(CubicVR::mat4::identity());
    waterfallPanel.draw();

    std::vector<DemodulatorInstance *> &demods = wxGetApp().getDemodMgr().getDemodulators();

    DemodulatorInstance *activeDemodulator = wxGetApp().getDemodMgr().getActiveDemodulator();
    DemodulatorInstance *lastActiveDemodulator = wxGetApp().getDemodMgr().getLastActiveDemodulator();

    bool isNew = shiftDown
            || (wxGetApp().getDemodMgr().getLastActiveDemodulator() && !wxGetApp().getDemodMgr().getLastActiveDemodulator()->isActive());

    int currentBandwidth = getBandwidth();
    long long currentCenterFreq = getCenterFrequency();

    ColorTheme *currentTheme = ThemeMgr::mgr.currentTheme;
    std::string last_type = wxGetApp().getDemodMgr().getLastDemodulatorType();

    if (mouseTracker.mouseInView() || wxGetApp().getDemodMgr().getActiveDemodulator()) {
        hoverAlpha += (1.0f-hoverAlpha)*0.1f;
        if (hoverAlpha > 1.5f) {
            hoverAlpha = 1.5f;
        }
        glContext->setHoverAlpha(hoverAlpha);
        if (nextDragState == WF_DRAG_RANGE) {
            float width = (1.0 / (float) ClientSize.x);
            float rangeWidth = mouseTracker.getOriginDeltaMouseX();
            float centerPos;

            if (mouseTracker.mouseDown()) {
                if (rangeWidth) {
                    width = rangeWidth;
                }
                centerPos = mouseTracker.getOriginMouseX() + width / 2.0;
            } else {
                centerPos = mouseTracker.getMouseX();
            }

            glContext->DrawDemod(lastActiveDemodulator, isNew?currentTheme->waterfallHighlight:currentTheme->waterfallDestroy, currentCenterFreq, currentBandwidth);

            if ((last_type == "LSB" || last_type == "USB") && mouseTracker.mouseDown()) {
                centerPos = mouseTracker.getMouseX();
                glContext->DrawRangeSelector(centerPos, centerPos-width, isNew?currentTheme->waterfallNew:currentTheme->waterfallHover);
            } else {
                glContext->DrawFreqSelector(centerPos, isNew?currentTheme->waterfallNew:currentTheme->waterfallHover, width, currentCenterFreq, currentBandwidth);
            }
        } else {
            if (lastActiveDemodulator) {
                glContext->DrawDemod(lastActiveDemodulator, ((isNew && activeDemodulator == NULL) || (activeDemodulator != NULL))?currentTheme->waterfallHighlight:currentTheme->waterfallDestroy, currentCenterFreq, currentBandwidth);
            }
            if (activeDemodulator == NULL) {
                glContext->DrawFreqSelector(mouseTracker.getMouseX(), ((isNew && lastActiveDemodulator) || (!lastActiveDemodulator) )?currentTheme->waterfallNew:currentTheme->waterfallHover, 0, currentCenterFreq, currentBandwidth);
            } else {
                glContext->DrawDemod(activeDemodulator, currentTheme->waterfallHover, currentCenterFreq, currentBandwidth);
            }
        }
    } else {
        hoverAlpha += (0.0f-hoverAlpha)*0.05f;
        if (hoverAlpha < 1.0e-5f) {
            hoverAlpha = 0;
        }
        glContext->setHoverAlpha(hoverAlpha);
        if (activeDemodulator) {
            glContext->DrawDemod(activeDemodulator, currentTheme->waterfallHighlight, currentCenterFreq, currentBandwidth);
        }
        if (lastActiveDemodulator) {
            glContext->DrawDemod(lastActiveDemodulator, currentTheme->waterfallHighlight, currentCenterFreq, currentBandwidth);
        }
    }

    glContext->setHoverAlpha(0);

    for (int i = 0, iMax = demods.size(); i < iMax; i++) {
        if (!demods[i]->isActive()) {
            continue;
        }
        if (activeDemodulator == demods[i] || lastActiveDemodulator == demods[i]) {
            continue;
        }
        glContext->DrawDemod(demods[i], currentTheme->waterfallHighlight, currentCenterFreq, currentBandwidth);
    }

    glContext->EndDraw();

    SwapBuffers();
    tex_update.unlock();
}

void WaterfallCanvas::OnKeyUp(wxKeyEvent& event) {
    InteractiveCanvas::OnKeyUp(event);
    shiftDown = event.ShiftDown();
    altDown = event.AltDown();
    ctrlDown = event.ControlDown();
    switch (event.GetKeyCode()) {
    case WXK_UP:
    case WXK_NUMPAD_UP:
            scaleMove = 0.0;
            zoom = 1.0;
            if (mouseZoom != 1.0) {
                mouseZoom = 0.95;
            }
        break;
    case WXK_DOWN:
    case WXK_NUMPAD_DOWN:
            scaleMove = 0.0;
            zoom = 1.0;
            if (mouseZoom != 1.0) {
                mouseZoom = 1.05;
            }
        break;
    case WXK_LEFT:
    case WXK_NUMPAD_LEFT:
    case WXK_RIGHT:
    case WXK_NUMPAD_RIGHT:
            freqMoving = false;
            break;
    }
}

void WaterfallCanvas::OnKeyDown(wxKeyEvent& event) {
    InteractiveCanvas::OnKeyDown(event);

    DemodulatorInstance *activeDemod = wxGetApp().getDemodMgr().getActiveDemodulator();

    long long originalFreq = getCenterFrequency();
    long long freq = originalFreq;

    switch (event.GetKeyCode()) {
    case WXK_UP:
    case WXK_NUMPAD_UP:
            if (!shiftDown) {
                mouseZoom = 1.0;
                zoom = 0.95;
            } else {
                scaleMove = 1.0;
            }
        break;
    case WXK_DOWN:
    case WXK_NUMPAD_DOWN:
            if (!shiftDown) {
                mouseZoom = 1.0;
                zoom = 1.05;
            } else {
                scaleMove = -1.0;
            }
        break;
    case WXK_RIGHT:
    case WXK_NUMPAD_RIGHT:
        if (isView) {
            freqMove = shiftDown?5.0:1.0;
            freqMoving = true;
        } else {
            freq += shiftDown?(getBandwidth() * 10):(getBandwidth() / 2);
        }
        break;
    case WXK_LEFT:
    case WXK_NUMPAD_LEFT:
        if (isView) {
            freqMove = shiftDown?-5.0:-1.0;
            freqMoving = true;
        } else {
            freq -= shiftDown?(getBandwidth() * 10):(getBandwidth() / 2);
        }
        break;
    case 'D':
    case WXK_DELETE:
        if (!activeDemod) {
            break;
        }
        wxGetApp().removeDemodulator(activeDemod);
        wxGetApp().getDemodMgr().deleteThread(activeDemod);
        break;
    case 'M':
        if (!activeDemod) {
            break;
        }
        activeDemod->setMuted(!activeDemod->isMuted());
        break;
    case 'B':
        if (spectrumCanvas) {
            spectrumCanvas->setShowDb(!spectrumCanvas->getShowDb());
        }
        break;
    case WXK_SPACE:
        wxGetApp().showFrequencyInput();
        break;
    case 'C':
        if (wxGetApp().getDemodMgr().getActiveDemodulator()) {
            wxGetApp().setFrequency(wxGetApp().getDemodMgr().getActiveDemodulator()->getFrequency());
        } else if (mouseTracker.mouseInView()) {
            long long freq = getFrequencyAt(mouseTracker.getMouseX());
            
            int snap = wxGetApp().getFrequencySnap();
            
            if (snap > 1) {
                freq = roundf((float)freq/(float)snap)*snap;
            }
            
            wxGetApp().setFrequency(freq);
        }
#ifdef USE_HAMLIB
        if (wxGetApp().rigIsActive() && (!wxGetApp().getRigThread()->getControlMode() || wxGetApp().getRigThread()->getCenterLock())) {
            wxGetApp().getRigThread()->setFrequency(wxGetApp().getFrequency(),true);
        }
#endif
        break;
    default:
        event.Skip();
        return;
    }

    long long minFreq = bandwidth/2;
    if (freq < minFreq) {
        freq = minFreq;
    }

    if (freq != originalFreq) {
        updateCenterFrequency(freq);
    }

}
void WaterfallCanvas::OnIdle(wxIdleEvent &event) {
    processInputQueue();
    Refresh();
    event.RequestMore();
}

void WaterfallCanvas::updateHoverState() {
    long long freqPos = getFrequencyAt(mouseTracker.getMouseX());
    
    std::vector<DemodulatorInstance *> *demodsHover = wxGetApp().getDemodMgr().getDemodulatorsAt(freqPos, 15000);
    
    wxGetApp().getDemodMgr().setActiveDemodulator(NULL);
    
    if (altDown) {
        nextDragState = WF_DRAG_RANGE;
        mouseTracker.setVertDragLock(true);
        mouseTracker.setHorizDragLock(false);
        if (shiftDown) {
            setStatusText("Click and drag to create a new demodulator by range.");
        } else {
            setStatusText("Click and drag to set the current demodulator range.");
        }
    } else if (demodsHover->size() && !shiftDown) {
        long near_dist = getBandwidth();
        
        DemodulatorInstance *activeDemodulator = NULL;
        
        for (int i = 0, iMax = demodsHover->size(); i < iMax; i++) {
            DemodulatorInstance *demod = (*demodsHover)[i];
            long long freqDiff = demod->getFrequency() - freqPos;
            long halfBw = (demod->getBandwidth() / 2);
            long long currentBw = getBandwidth();
            long long globalBw = wxGetApp().getSampleRate();
            long dist = abs(freqDiff);
            double bufferBw = 10000.0 * ((double)currentBw / (double)globalBw);
            double maxDist = ((double)halfBw + bufferBw);
            
            if ((double)dist <= maxDist) {
                if ((freqDiff > 0 && demod->getDemodulatorType() == "USB") ||
                    (freqDiff < 0 && demod->getDemodulatorType() == "LSB")) {
                    continue;
                }
                
                if (dist < near_dist) {
                    activeDemodulator = demod;
                    near_dist = dist;
                }
                
                long edge_dist = abs(halfBw - dist);
                if (edge_dist < near_dist) {
                    activeDemodulator = demod;
                    near_dist = edge_dist;
                }
            }
        }
        
        if (activeDemodulator == NULL) {
            nextDragState = WF_DRAG_NONE;
            SetCursor(wxCURSOR_CROSS);
            return;
        }
        
        wxGetApp().getDemodMgr().setActiveDemodulator(activeDemodulator);
        
        long long freqDiff = activeDemodulator->getFrequency() - freqPos;
        
        if (abs(freqDiff) > (activeDemodulator->getBandwidth() / 3)) {
            
            if (freqDiff > 0) {
                if (activeDemodulator->getDemodulatorType() != "USB") {
                    nextDragState = WF_DRAG_BANDWIDTH_LEFT;
                    SetCursor(wxCURSOR_SIZEWE);
                }
            } else {
                if (activeDemodulator->getDemodulatorType() != "LSB") {
                    nextDragState = WF_DRAG_BANDWIDTH_RIGHT;
                    SetCursor(wxCURSOR_SIZEWE);
                }
            }
            
            mouseTracker.setVertDragLock(true);
            mouseTracker.setHorizDragLock(false);
            setStatusText("Click and drag to change demodulator bandwidth. SPACE or numeric key for direct frequency input. [, ] to nudge, M for mute, D to delete, C to center.");
        } else {
            SetCursor(wxCURSOR_SIZING);
            nextDragState = WF_DRAG_FREQUENCY;
            
            mouseTracker.setVertDragLock(true);
            mouseTracker.setHorizDragLock(false);
            setStatusText("Click and drag to change demodulator frequency; SPACE or numeric key for direct input. [, ] to nudge, M for mute, D to delete, C to center.");
        }
    } else {
        SetCursor(wxCURSOR_CROSS);
        nextDragState = WF_DRAG_NONE;
        if (shiftDown) {
            setStatusText("Click to create a new demodulator or hold ALT to drag range, SPACE or numeric key for direct center frequency input.");
        } else {
            setStatusText(
                          "Click to set active demodulator frequency or hold ALT to drag range; hold SHIFT to create new.  Right drag or wheel to Zoom.  Arrow keys to navigate/zoom, C to center.");
        }
    }
    
    delete demodsHover;
}

void WaterfallCanvas::OnMouseMoved(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseMoved(event);
    DemodulatorInstance *demod = wxGetApp().getDemodMgr().getActiveDemodulator();

    if (mouseTracker.mouseDown()) {
        if (demod == NULL) {
            return;
        }
        if (dragState == WF_DRAG_BANDWIDTH_LEFT || dragState == WF_DRAG_BANDWIDTH_RIGHT) {

            int bwDiff = (int) (mouseTracker.getDeltaMouseX() * (float) getBandwidth()) * 2;

            if (dragState == WF_DRAG_BANDWIDTH_LEFT) {
                bwDiff = -bwDiff;
            }

            int currentBW = dragBW;

            currentBW = currentBW + bwDiff;
            if (currentBW > CHANNELIZER_RATE_MAX) {
                currentBW = CHANNELIZER_RATE_MAX;
            }
            if (currentBW < MIN_BANDWIDTH) {
                currentBW = MIN_BANDWIDTH;
            }

            demod->setBandwidth(currentBW);
            dragBW = currentBW;
        }

        if (dragState == WF_DRAG_FREQUENCY) {
            long long bwTarget = getFrequencyAt(mouseTracker.getMouseX()) - dragOfs;
            long long currentFreq = demod->getFrequency();
            long long bwDiff = bwTarget - currentFreq;
            int snap = wxGetApp().getFrequencySnap();

            if (snap > 1) {
                bwDiff = roundf((float)bwDiff/(float)snap)*snap;
            }

            if (bwDiff) {
                demod->setFrequency(currentFreq + bwDiff);
                if (demod->isDeltaLock()) {
                    demod->setDeltaLockOfs(demod->getFrequency() - wxGetApp().getFrequency());
                }
                currentFreq = demod->getFrequency();
                demod->updateLabel(currentFreq);
            }
        }
    } else if (mouseTracker.mouseRightDown()) {
        mouseZoom = mouseZoom + ((1.0 - (mouseTracker.getDeltaMouseY() * 4.0)) - mouseZoom) * 0.1;
    } else {
        updateHoverState();
    }
}

void WaterfallCanvas::OnMouseDown(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseDown(event);

    updateHoverState();
    dragState = nextDragState;
    wxGetApp().getDemodMgr().updateLastState();

    if (dragState && dragState != WF_DRAG_RANGE) {
        DemodulatorInstance *demod = wxGetApp().getDemodMgr().getActiveDemodulator();
        if (demod) {
            dragOfs = (long long) (mouseTracker.getMouseX() * (float) getBandwidth()) + getCenterFrequency() - (getBandwidth() / 2) - demod->getFrequency();
            dragBW = demod->getBandwidth();
        }
        wxGetApp().getDemodMgr().setActiveDemodulator(wxGetApp().getDemodMgr().getActiveDemodulator(), false);
    }
}

void WaterfallCanvas::OnMouseWheelMoved(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseWheelMoved(event);
    float movement = (float)event.GetWheelRotation() / (float)event.GetLinesPerAction();

    mouseZoom = 1.0f - movement/1000.0f;
}

void WaterfallCanvas::OnMouseReleased(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseReleased(event);
    wxGetApp().getDemodMgr().updateLastState();

    bool isNew = shiftDown || (wxGetApp().getDemodMgr().getLastActiveDemodulator() == NULL)
            || (wxGetApp().getDemodMgr().getLastActiveDemodulator() && !wxGetApp().getDemodMgr().getLastActiveDemodulator()->isActive());

    mouseTracker.setVertDragLock(false);
    mouseTracker.setHorizDragLock(false);

    DemodulatorInstance *demod = isNew?NULL:wxGetApp().getDemodMgr().getLastActiveDemodulator();
    DemodulatorInstance *activeDemod = isNew?NULL:wxGetApp().getDemodMgr().getActiveDemodulator();

    DemodulatorMgr *mgr = &wxGetApp().getDemodMgr();

    if (mouseTracker.getOriginDeltaMouseX() == 0 && mouseTracker.getOriginDeltaMouseY() == 0) {
        float pos = mouseTracker.getMouseX();
        long long input_center_freq = getCenterFrequency();
        long long freqTarget = input_center_freq - (long long) (0.5 * (float) getBandwidth()) + (long long) ((float) pos * (float) getBandwidth());
        long long demodFreq = demod?demod->getFrequency():freqTarget;
        long long bwDiff = freqTarget - demodFreq;
        long long freq = demodFreq;

         int snap = wxGetApp().getFrequencySnap();

         if (snap > 1) {
             if (demod) {
                 bwDiff = roundf((double)bwDiff/(double)snap)*snap;
                 freq += bwDiff;
             } else {
                 freq = roundl((long double)freq/(double)snap)*snap;
             }
         } else {
             freq += bwDiff;
         }


        if (dragState == WF_DRAG_NONE) {
            if (!isNew && wxGetApp().getDemodMgr().getDemodulators().size()) {
                mgr->updateLastState();
                demod = wxGetApp().getDemodMgr().getLastActiveDemodulator();
            } else {
                isNew = true;
                demod = wxGetApp().getDemodMgr().newThread();
                demod->setFrequency(freq);

                demod->setDemodulatorType(mgr->getLastDemodulatorType());
                demod->setBandwidth(mgr->getLastBandwidth());
                demod->setSquelchLevel(mgr->getLastSquelchLevel());
                demod->setSquelchEnabled(mgr->isLastSquelchEnabled());
                demod->setGain(mgr->getLastGain());
                demod->setMuted(mgr->isLastMuted());
                if (mgr->getLastDeltaLock()) {
                    demod->setDeltaLock(true);
                    demod->setDeltaLockOfs(wxGetApp().getFrequency()-freq);
                } else {
                    demod->setDeltaLock(false);
                }
                demod->writeModemSettings(mgr->getLastModemSettings(mgr->getLastDemodulatorType()));
                demod->run();

                wxGetApp().bindDemodulator(demod);
            }

            if (!demod) {
                dragState = WF_DRAG_NONE;
                return;
            }

            demod->updateLabel(freq);
            demod->setFrequency(freq);
            if (demod->isDeltaLock()) {
                demod->setDeltaLockOfs(demod->getFrequency() - wxGetApp().getFrequency());
            }
  
            if (isNew) {
                setStatusText("New demodulator at frequency: %s", freq);
            } else {
                setStatusText("Moved demodulator to frequency: %s", freq);
            }

            wxGetApp().getDemodMgr().setActiveDemodulator(demod, false);
          SetCursor(wxCURSOR_SIZING);
            nextDragState = WF_DRAG_FREQUENCY;
            mouseTracker.setVertDragLock(true);
            mouseTracker.setHorizDragLock(false);
        } else {
            if (activeDemod) {
                wxGetApp().getDemodMgr().setActiveDemodulator(activeDemod, false);
                mgr->updateLastState();
                activeDemod->setTracking(true);
                nextDragState = WF_DRAG_FREQUENCY;
            } else {
                nextDragState = WF_DRAG_NONE;
            }
        }
    } else if (dragState == WF_DRAG_RANGE) {
        float width = mouseTracker.getOriginDeltaMouseX();

        float pos;
        std::string last_type = mgr->getLastDemodulatorType();

        if (last_type == "LSB" || last_type == "USB") {
            float pos1 = mouseTracker.getOriginMouseX();
            float pos2 = mouseTracker.getMouseX();

            if (pos2 < pos1) {
                float tmp = pos1;
                pos1 = pos2;
                pos2 = tmp;
            }

            pos = (last_type == "LSB")?pos2:pos1;
            width *= 2;
        } else {
            pos = mouseTracker.getOriginMouseX() + width / 2.0;
        }

        long long input_center_freq = getCenterFrequency();
        long long freq = input_center_freq - (long long) (0.5 * (float) getBandwidth()) + (long long) ((float) pos * (float) getBandwidth());
        unsigned int bw = (unsigned int) (fabs(width) * (float) getBandwidth());

        if (bw < MIN_BANDWIDTH) {
            bw = MIN_BANDWIDTH;
        }

        if (!bw) {
            dragState = WF_DRAG_NONE;
            return;
        }

        int snap = wxGetApp().getFrequencySnap();

        if (snap > 1) {
            freq = roundl((long double)freq/(double)snap)*snap;
        }


        if (!isNew && wxGetApp().getDemodMgr().getDemodulators().size()) {
            mgr->updateLastState();
            demod = wxGetApp().getDemodMgr().getLastActiveDemodulator();
        } else {
            demod = wxGetApp().getDemodMgr().newThread();
            demod->setFrequency(freq);
            demod->setDemodulatorType(mgr->getLastDemodulatorType());
            demod->setBandwidth(bw);
            demod->setSquelchLevel(mgr->getLastSquelchLevel());
            demod->setSquelchEnabled(mgr->isLastSquelchEnabled());
            demod->setGain(mgr->getLastGain());
            demod->setMuted(mgr->isLastMuted());
            if (mgr->getLastDeltaLock()) {
                demod->setDeltaLock(true);
                demod->setDeltaLockOfs(wxGetApp().getFrequency()-freq);
            } else {
                demod->setDeltaLock(false);
            }
            demod->writeModemSettings(mgr->getLastModemSettings(mgr->getLastDemodulatorType()));

            demod->run();

            wxGetApp().bindDemodulator(demod);
        }

        if (demod == NULL) {
            dragState = WF_DRAG_NONE;
            return;
        }

        setStatusText("New demodulator at frequency: %s", freq);

        demod->updateLabel(freq);
        demod->setFrequency(freq);
        demod->setBandwidth(bw);
        mgr->setActiveDemodulator(demod, false);
        mgr->updateLastState();
    }

    dragState = WF_DRAG_NONE;
}

void WaterfallCanvas::OnMouseLeftWindow(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseLeftWindow(event);
    SetCursor(wxCURSOR_CROSS);
    wxGetApp().getDemodMgr().setActiveDemodulator(NULL);
    mouseZoom = 1.0;
}

void WaterfallCanvas::OnMouseEnterWindow(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseEnterWindow(event);
    SetCursor(wxCURSOR_CROSS);
}

void WaterfallCanvas::OnMouseRightDown(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseRightDown(event);

    SetCursor(wxCURSOR_SIZENS);
    mouseTracker.setVertDragLock(true);
    mouseTracker.setHorizDragLock(true);
}

void WaterfallCanvas::OnMouseRightReleased(wxMouseEvent& event) {
    InteractiveCanvas::OnMouseRightReleased(event);
    SetCursor(wxCURSOR_CROSS);
    mouseTracker.setVertDragLock(false);
    mouseTracker.setHorizDragLock(false);
    mouseZoom = 1.0;
}

SpectrumVisualDataQueue *WaterfallCanvas::getVisualDataQueue() {
    return &visualDataQueue;
}

void WaterfallCanvas::updateCenterFrequency(long long freq) {
    if (isView) {
        setView(freq, getBandwidth());
        if (spectrumCanvas) {
            spectrumCanvas->setView(freq, getBandwidth());
        }
        
        long long minFreq = wxGetApp().getFrequency()-(wxGetApp().getSampleRate()/2);
        long long maxFreq = wxGetApp().getFrequency()+(wxGetApp().getSampleRate()/2);
        
        if (freq - bandwidth / 2 < minFreq) {
            wxGetApp().setFrequency(wxGetApp().getFrequency() - (minFreq - (freq - bandwidth/2)));
        }
        if (freq + bandwidth / 2 > maxFreq) {
            wxGetApp().setFrequency(wxGetApp().getFrequency() + ((freq + bandwidth/2) - maxFreq));
        }
    } else {
        if (spectrumCanvas) {
            spectrumCanvas->setCenterFrequency(freq);
        }
        wxGetApp().setFrequency(freq);
    }

}

void WaterfallCanvas::setLinesPerSecond(int lps) {
    tex_update.lock();
    linesPerSecond = lps;
    while (!visualDataQueue.empty()) {
        SpectrumVisualData *vData;
        visualDataQueue.pop(vData);
        
        if (vData) {
            vData->decRefCount();
        }
    }
    tex_update.unlock();
}

void WaterfallCanvas::setMinBandwidth(int min) {
    minBandwidth = min;
}
