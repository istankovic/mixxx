#include <QDebug>
#include <QStylePainter>
#include <QPaintEvent>
#include <QStyleOptionSlider>

#include "library/basesqltablemodel.h"
#include "library/columncache.h"
#include "util/stringhelper.h"

#include "wminiviewscrollbar.h"

namespace {
float interpolSize(float current, float max1, float max2) {
    float res = current*(max2/max1);
    return res;
}
}

WMiniViewScrollBar::WMiniViewScrollBar(QWidget* parent)
        : QScrollBar(parent),
          m_sortColumn(-1),
          m_dataRole(Qt::DisplayRole),
          m_showLetters(true) {
    setMouseTracking(true);
    
    connect(this, SIGNAL(rangeChanged(int,int)),
            this, SLOT(triggerUpdate()));
}

void WMiniViewScrollBar::setShowLetters(bool show) {
    m_showLetters = show;
}

bool WMiniViewScrollBar::showLetters() const {
    return m_showLetters;
}

void WMiniViewScrollBar::setSortColumn(int column) {
    m_sortColumn = column;
    triggerUpdate();
}

int WMiniViewScrollBar::sortColumn() const {
    return m_sortColumn;
}

void WMiniViewScrollBar::setTreeView(QPointer<QTreeView> pTreeView) {
    m_pTreeView = pTreeView;
}

QPointer<QTreeView> WMiniViewScrollBar::getTreeView() {
    return m_pTreeView;
}

void WMiniViewScrollBar::setRole(int role) {
    m_dataRole = role;
    triggerUpdate();
}

int WMiniViewScrollBar::role() const {
    return m_dataRole;
}

void WMiniViewScrollBar::setModel(QAbstractItemModel* model) {
    m_pModel = model;
    if (!m_pModel.isNull()) {
        connect(m_pModel, SIGNAL(dataChanged(const QModelIndex&, const QModelIndex&)),
                this, SLOT(triggerUpdate()));
        
        triggerUpdate();
    }
}

void WMiniViewScrollBar::paintEvent(QPaintEvent* event) {
    if (!m_showLetters) {
        QScrollBar::paintEvent(event);
        return;
    }
    
    QStylePainter painter(this);
    QStyleOptionSlider opt(getStyleOptions());
    painter.drawComplexControl(QStyle::CC_ScrollBar, opt);
    
    painter.setBrush(palette().color(QPalette::Text));
    int flags = Qt::AlignTop | Qt::AlignHCenter;

    // Get total size
    int letterSize = fontMetrics().height();
    int w = width();
    const QPoint bottom(w, letterSize);
    
    // Draw each letter in its position
    for (const CharPosition& p : m_computedPosition) {
        if (p.position < 0 || p.character.isNull()) {
            continue;
        }
        QFont f(font());
        f.setBold(p.bold);
        painter.setFont(f);
        
        QPoint topLeft = QPoint(0, p.position);
        painter.drawText(QRect(topLeft, topLeft + bottom), flags, p.character);
    }
    
    opt.rect = style()->subControlRect(QStyle::CC_ScrollBar, &opt, 
                                       QStyle::SC_ScrollBarSlider, this);
    opt.subControls = QStyle::SC_ScrollBarSlider;
    opt.activeSubControls = QStyle::SC_ScrollBarSlider;
    painter.drawControl(QStyle::CE_ScrollBarSlider, opt);
    painter.drawControl(QStyle::CE_ScrollBarAddLine, opt);
    painter.drawControl(QStyle::CE_ScrollBarSubLine, opt);
}

void WMiniViewScrollBar::resizeEvent(QResizeEvent* pEvent) {
    computeLettersSize();
    QScrollBar::resizeEvent(pEvent);
}

void WMiniViewScrollBar::mouseMoveEvent(QMouseEvent* pEvent) {
    // Check mouse hover condition
    
    bool found = false;
    int letterSize = fontMetrics().height();
    int posVert = pEvent->pos().y();

    for (CharPosition& c : m_computedPosition) {
        if (posVert >= c.position && posVert < c.position + letterSize && !found) {
            c.bold = true;
            found = true;
        } else {
            c.bold = false;
        }
    } 
    update();
    QScrollBar::mouseMoveEvent(pEvent);
}

void WMiniViewScrollBar::mousePressEvent(QMouseEvent* pEvent) {
    QScrollBar::mousePressEvent(pEvent);
    
    bool found = false;
    auto itL = m_letters.begin();
    auto itC = m_computedPosition.begin();
    int totalSum = 0;
    
    // When setting the slider position the correct position is the sum of all
    // the previous elements of the selected element. 
    // In this scrollbar maximum = model.size(), minimum = 0
    while (!found && itL != m_letters.end() && itC != m_computedPosition.end()) {
        if (itC->bold) {
            found = true;
            setSliderPosition(totalSum);
        } else {
            totalSum += itL->count;
        }
        ++itL;
        ++itC;
    }
}

void WMiniViewScrollBar::leaveEvent(QEvent* pEvent) {
    for (CharPosition& c : m_computedPosition) {
        c.bold = false;
    }
    QScrollBar::leaveEvent(pEvent);
}

void WMiniViewScrollBar::refreshCharMap() {    
    if (m_pModel.isNull()) {
        return;
    }
    
    int size = m_pModel->rowCount();
    setMinimum(0);
    setMaximum(size);
    const QModelIndex& rootIndex = m_pModel->index(0, 0);
    
    m_letters.clear();
    if (!isValidColumn()) {
        return;
    }
    
    for (int i = 0; i < size; ++i) {
        const QModelIndex& index = rootIndex.sibling(i, m_sortColumn);
        QString text = index.data(m_dataRole).toString();
        int count = getVisibleChildCount(index);
        QChar c;
        
        if (m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR) && 
            text.size() >= 3) {
            c = text.at(2);
        } else {
            c = StringHelper::getFirstCharForGrouping(text);
        }
        addToLastCharCount(c, count);
    }
}

void WMiniViewScrollBar::computeLettersSize() {
    // Initialize each position with the times each character appears
    m_computedPosition.resize(m_letters.size());
    for (int i = 0; i < m_letters.size(); ++i) {
        m_computedPosition[i].bold = false;
        m_computedPosition[i].position = m_letters[i].count;
        m_computedPosition[i].character = m_letters[i].character;
    }
    
    QStyleOptionSlider opt(getStyleOptions());
    QRect grooveRect = 
            style()->subControlRect(QStyle::CC_ScrollBar, &opt,
                                    QStyle::SC_ScrollBarGroove, this);
            
    // Height of a letter
    const int letterSize = fontMetrics().height();
    const int totalLinearSize = grooveRect.bottom();
    float nextAvailableScrollPosition = grooveRect.top();
    float optimalScrollPosition = grooveRect.top();
    
    int totalCount = 0;
    // Get the total count of letters appearance to make a linear interpolation
    // with the current widget height
    for (const CharCount& p : m_letters) {
        totalCount += p.count;
    }
    
    for (CharPosition& p : m_computedPosition) {
        float size = interpolSize(p.position, totalCount, totalLinearSize);
        float percentage = 100.0*p.position/float(totalCount);
        
        if ((optimalScrollPosition + size) < (nextAvailableScrollPosition + letterSize) ||
                percentage < 1.0 || 
                (optimalScrollPosition + letterSize) > totalLinearSize) {
            // If a very small percentage letter takes the space for a letter
            // with more high ocupacy percentage this can be annoying
            
            // A negative position won't paint the character
            p.position = -1;
        } else {
            p.position = (int) optimalScrollPosition;
            nextAvailableScrollPosition = optimalScrollPosition + (float)(letterSize);
        }
        optimalScrollPosition += size;
    }
}

void WMiniViewScrollBar::triggerUpdate() {
    refreshCharMap();
    computeLettersSize();
    update();
}

QStyleOptionSlider WMiniViewScrollBar::getStyleOptions() {
    QStyleOptionSlider opt;
    opt.init(this);
    opt.subControls = QStyle::SC_ScrollBarAddLine | QStyle::SC_ScrollBarSubLine;
    opt.activeSubControls = QStyle::SC_ScrollBarAddLine | QStyle::SC_ScrollBarSubLine;
    opt.orientation = orientation();
    opt.minimum = minimum();
    opt.maximum = maximum();
    opt.sliderPosition = sliderPosition();
    opt.sliderValue = value();
    opt.singleStep = singleStep();
    opt.pageStep = pageStep();
    
    return opt;
}

int WMiniViewScrollBar::getFieldIndex(ColumnCache::Column col) {
    QPointer<BaseSqlTableModel> pModel = qobject_cast<BaseSqlTableModel*>(m_pModel);
    if (pModel.isNull()) {
        return -1;
    }
    
    return pModel->fieldIndex(col);
}

bool WMiniViewScrollBar::isValidColumn() {
    return m_sortColumn == 0 || 
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR) ||
        m_sortColumn == getFieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_ARTIST) ||
            m_sortColumn == getFieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_TITLE);
}

void WMiniViewScrollBar::addToLastCharCount(const QChar& c, int sum) {
    if (m_letters.size() <= 0) {
        m_letters.append({c, sum});
    } else {
        CharCount& lastC = m_letters.last();
        if (lastC.character == c) {
            lastC.count += sum;
        } else {
            m_letters.append({c, sum});
        }
    }
}

int WMiniViewScrollBar::getVisibleChildCount(const QModelIndex& index) {
    if (!index.isValid()) {
        return 0;
    }
    
    if (m_pTreeView.isNull()) {
        return 1;
    }
    
    if (!m_pTreeView->isExpanded(index)) {
        return 1;
    }
    
    int total = 1;
    int rowCount = m_pModel->rowCount(index);
    for (int i = 0; i < rowCount; ++i) {
        total += getVisibleChildCount(index.child(i, m_sortColumn));
    }
    return total;
}