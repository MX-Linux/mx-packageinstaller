#include "checkableheaderview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionButton>
#include <QResizeEvent>

CheckableHeaderView::CheckableHeaderView(Qt::Orientation orientation, QWidget *parent)
    : QHeaderView(orientation, parent) {
    setSectionsClickable(true);
    m_box = new QCheckBox(this);
    m_box->setTristate(false);
    m_box->hide();
    connect(m_box, &QCheckBox::toggled, this, [this](bool on){ m_checked = on; emit toggled(on); });
    connect(this, &QHeaderView::sectionResized, this, [this](int, int, int){ updateCheckboxGeometry(); });
}

void CheckableHeaderView::setTargetColumn(int column) {
    m_targetColumn = column;
    updateCheckboxGeometry();
}

void CheckableHeaderView::setCheckboxVisible(bool visible) {
    if (m_visible == visible) return;
    m_visible = visible;
    if (m_box) {
        m_box->setVisible(visible);
        updateCheckboxGeometry();
    }
}

void CheckableHeaderView::setChecked(bool checked) {
    if (m_checked == checked) return;
    m_checked = checked;
    if (m_box) {
        const QSignalBlocker blocker(m_box);
        m_box->setChecked(checked);
    }
}

void CheckableHeaderView::paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const {
    QHeaderView::paintSection(painter, rect, logicalIndex);
    Q_UNUSED(painter);
    Q_UNUSED(rect);
    Q_UNUSED(logicalIndex);
}

void CheckableHeaderView::resizeEvent(QResizeEvent *event) {
    QHeaderView::resizeEvent(event);
    updateCheckboxGeometry();
}

QSize CheckableHeaderView::sectionSizeFromContents(int logicalIndex) const {
    QSize base = QHeaderView::sectionSizeFromContents(logicalIndex);
    if (m_visible && logicalIndex == m_targetColumn) {
        // Ensure the section is at least big enough for a checkbox indicator
        QStyleOptionButton opt;
        const int indicatorW = style()->pixelMetric(QStyle::PM_IndicatorWidth, &opt, this);
        const int indicatorH = style()->pixelMetric(QStyle::PM_IndicatorHeight, &opt, this);
        base.setWidth(qMax(base.width(), indicatorW + 6));
        base.setHeight(qMax(base.height(), indicatorH + 6));
    }
    return base;
}

bool CheckableHeaderView::viewportEvent(QEvent *event) {
    // Horizontal scrolling (setOffset()) only scrolls the viewport's contents and emits
    // no signal; the box is a child of the header, not of the viewport, so follow the
    // offset from the repaint that scrolling triggers.
    if (event->type() == QEvent::Paint && offset() != m_lastOffset) {
        m_lastOffset = offset();
        updateCheckboxGeometry();
    }
    return QHeaderView::viewportEvent(event);
}

void CheckableHeaderView::updateCheckboxGeometry() {
    if (!m_box) return;
    if (!m_visible) { m_box->hide(); return; }
    if (orientation() != Qt::Horizontal) return;

    int x = sectionViewportPosition(m_targetColumn);
    int w = sectionSize(m_targetColumn);
    int h = height();

    // Place checkbox centered in the header section
    QStyleOptionButton opt;
    const int indicatorW = style()->pixelMetric(QStyle::PM_IndicatorWidth, &opt, this);
    const int indicatorH = style()->pixelMetric(QStyle::PM_IndicatorHeight, &opt, this);
    int cbx = x + (w - indicatorW) / 2;
    int cby = (h - indicatorH) / 2;
    m_box->setGeometry(cbx, cby, indicatorW, indicatorH);
    // Hide it while the target section is scrolled out of view.
    m_box->setVisible(x + w > 0 && x < viewport()->width());
}
