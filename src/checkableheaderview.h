// A QHeaderView with an optional checkbox painted in a target section (column)
#pragma once

#include <QHeaderView>
#include <QCheckBox>

class CheckableHeaderView : public QHeaderView {
    Q_OBJECT

public:
    explicit CheckableHeaderView(Qt::Orientation orientation, QWidget *parent = nullptr);

    void setTargetColumn(int column);
    void setCheckboxVisible(bool visible);
    void setChecked(bool checked);
    [[nodiscard]] bool isChecked() const { return m_checked; }
    [[nodiscard]] bool isCheckboxVisible() const { return m_visible; }

signals:
    void toggled(bool checked);

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
    void resizeEvent(QResizeEvent *event) override;
    QSize sectionSizeFromContents(int logicalIndex) const override;
    bool viewportEvent(QEvent *event) override;

private:
    void updateCheckboxGeometry();

    int m_targetColumn {0};
    int m_lastOffset {0};
    bool m_checked {false};
    bool m_visible {false};
    QCheckBox *m_box {nullptr};
};
