// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "bitcoinunits.h"
#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "transactionfilterproxy.h"
#include "transactiontablemodel.h"
#include "main.h"
#include "hybridui/styleSheet.h"
#ifdef WIN32
#include <string.h>
#endif

#include "util.h"
#include "compat.h"

#include <QAbstractItemDelegate>
#include <QPainter>

#define NUM_ITEMS 5

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle *_platformStyle, QObject *parent=nullptr):
        QAbstractItemDelegate(parent), unit(BitcoinUnits::BTC),
        platformStyle(_platformStyle)
    {
        background_color_selected = GetStringStyleValue("txviewdelegate/background-color-selected", "#009ee5");
        background_color = GetStringStyleValue("txviewdelegate/background-color", "#393939");
        alternate_background_color = GetStringStyleValue("txviewdelegate/alternate-background-color", "#2e2e2e");
        foreground_color = GetStringStyleValue("txviewdelegate/foreground-color", "#dedede");
        foreground_color_selected = GetStringStyleValue("txviewdelegate/foreground-color-selected", "#ffffff");
        amount_color = GetStringStyleValue("txviewdelegate/amount-color", "#ffffff");
        color_unconfirmed = GetColorStyleValue("guiconstants/color-unconfirmed", COLOR_UNCONFIRMED);
        color_negative = GetColorStyleValue("guiconstants/color-negative", COLOR_NEGATIVE);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();
        bool selected = option.state & QStyle::State_Selected;
        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        if(selected)
        {
            icon = PlatformStyle::SingleColorIcon(icon, foreground_color_selected);
        }
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();

        QModelIndex ind = index.model()->index(index.row(), TransactionTableModel::Type, index.parent());
        QString typeString = ind.data(Qt::DisplayRole).toString();

        QRect mainRect = option.rect;
        QColor txColor = index.row() % 2 ? background_color : alternate_background_color;
        painter->fillRect(mainRect, txColor);

        if(selected)
        {
            painter->fillRect(mainRect.x()+1, mainRect.y()+1, mainRect.width()-2, mainRect.height()-2, background_color_selected);
        }

        QColor foreground = foreground_color;
        if(selected)
        {
            foreground = foreground_color_selected;
        }
        painter->setPen(foreground);

        QRect dateRect(mainRect.left() + MARGIN, mainRect.top(), DATE_WIDTH, TX_SIZE);
        painter->drawText(dateRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        int topMargin = (TX_SIZE - DECORATION_SIZE) / 2;
        QRect decorationRect(dateRect.topRight() + QPoint(MARGIN, topMargin), QSize(DECORATION_SIZE, DECORATION_SIZE));
        icon.paint(painter, decorationRect);

        QRect typeRect(decorationRect.right() + MARGIN, mainRect.top(), TYPE_WIDTH, TX_SIZE);
        painter->drawText(typeRect, Qt::AlignLeft|Qt::AlignVCenter, typeString);

        bool watchOnly = index.data(TransactionTableModel::WatchonlyRole).toBool();

        if (watchOnly)
        {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            if(selected)
            {
                iconWatchonly = PlatformStyle::SingleColorIcon(iconWatchonly, foreground_color_selected);
            }
            QRect watchonlyRect(typeRect.right() + MARGIN, mainRect.top() + topMargin, DECORATION_SIZE, DECORATION_SIZE);
            iconWatchonly.paint(painter, watchonlyRect);
        }

        int addressMargin = watchOnly ? MARGIN + 20 : MARGIN;
        int addressWidth = mainRect.width() - DATE_WIDTH - DECORATION_SIZE - TYPE_WIDTH - AMOUNT_WIDTH - 5*MARGIN;
        addressWidth = watchOnly ? addressWidth - 20 : addressWidth;

        QFont addressFont = option.font;
        addressFont.setPointSizeF(addressFont.pointSizeF() * 0.95);
        painter->setFont(addressFont);

        QFontMetrics fmName(painter->font());
        QString clippedAddress = fmName.elidedText(address, Qt::ElideRight, addressWidth);

        QRect addressRect(typeRect.right() + addressMargin, mainRect.top(), addressWidth, TX_SIZE);
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, clippedAddress);

        QFont amountFont = option.font;
        painter->setFont(amountFont);

        if(amount < 0)
        {
            foreground = color_negative;
        }
        else if(!confirmed)
        {
            foreground = color_unconfirmed;
        }
        else
        {
            foreground = amount_color;
        }

        if(selected)
        {
            foreground = foreground_color_selected;
        }
        painter->setPen(foreground);

        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::separatorAlways);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amountRect(addressRect.right() + MARGIN, addressRect.top(), AMOUNT_WIDTH, TX_SIZE);
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(TX_SIZE, TX_SIZE);
    }

    int unit;
    const PlatformStyle *platformStyle;

private:
    QColor background_color_selected;
    QColor background_color;
    QColor alternate_background_color;
    QColor foreground_color;
    QColor foreground_color_selected;
    QColor amount_color;
    QColor color_unconfirmed;
    QColor color_negative;
};

#include "overviewpage.moc"

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    currentBalance(-1),
    currentUnconfirmedBalance(-1),
    currentImmatureBalance(-1),
    currentStake(-1),
    currentWatchOnlyBalance(-1),
    currentWatchUnconfBalance(-1),
    currentWatchImmatureBalance(-1),
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);
    // Set stylesheet
    SetObjectStyleSheet(ui->labelWalletStatus, StyleSheetNames::ButtonTransparent);
    SetObjectStyleSheet(ui->labelTransactionsStatus, StyleSheetNames::ButtonTransparent);

    // read config
    bool torEnabled;
    if(mapArgs.count("-torsetup")){
        torEnabled = GetBoolArg("-torsetup", DEFAULT_TOR_SETUP);
    }else{
        torEnabled = settings.value("fTorSetup").toBool();
    }

    if(torEnabled){
        ui->checkboxEnabledTor->setChecked(true);
    }else{
        ui->checkboxEnabledTor->setChecked(false);
    }

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = platformStyle->SingleColorIcon(":/icons/warning");
    icon.addPixmap(icon.pixmap(QSize(64,64), QIcon::Normal), QIcon::Disabled); // also set the disabled icon because we are using a disabled QPushButton to work around missing HiDPI support of QLabel (https://bugreports.qt.io/browse/QTBUG-42503)
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));
    connect(ui->checkboxEnabledTor, SIGNAL(toggled(bool)), this, SLOT(handleEnabledTorChanged()));

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::handleEnabledTorChanged(){

	QMessageBox msgBox;

	if(ui->checkboxEnabledTor->isChecked()){
        settings.setValue("fTorSetup", true);
        msgBox.setText("Please restart the Index wallet to route your connection through Tor to protect your IP address. \nSyncing your wallet might be slower with TOR. \nNote that -torsetup in index.conf will always override any changes made here.");
	}else{
        settings.setValue("fTorSetup", false);
        msgBox.setText("Please restart the Index wallet to disable routing of your connection through Tor to protect your IP address. \nNote that -torsetup in index.conf will always override any changes made here.");
	}
	msgBox.exec();
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance,const CAmount& stake, const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentStake = stake;
    currentWatchOnlyBalance = watchOnlyBalance;
    currentWatchUnconfBalance = watchUnconfBalance;
    currentWatchImmatureBalance = watchImmatureBalance;
    CAmount totalbal = balance + unconfirmedBalance + immatureBalance + currentSigmaBalance + currentSigmaUnconfirmedBalance + currentStake;
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, currentBalance, false, BitcoinUnits::separatorAlways));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance, false, BitcoinUnits::separatorAlways));
    ui->labelStakeBalance->setText(BitcoinUnits::formatWithUnit(unit, currentStake, false, BitcoinUnits::separatorAlways));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance, false, BitcoinUnits::separatorAlways));
    ui->labelTotal->setText(BitcoinUnits::formatWithUnit(unit, (totalbal > 7446) ? totalbal:0, false, BitcoinUnits::separatorAlways));
    ui->labelWatchAvailable->setText(BitcoinUnits::formatWithUnit(unit, watchOnlyBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchPending->setText(BitcoinUnits::formatWithUnit(unit, watchUnconfBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchImmature->setText(BitcoinUnits::formatWithUnit(unit, watchImmatureBalance, false, BitcoinUnits::separatorAlways));
    ui->labelWatchTotal->setText(BitcoinUnits::formatWithUnit(unit, watchOnlyBalance + watchUnconfBalance + watchImmatureBalance, false, BitcoinUnits::separatorAlways));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    bool showWatchOnlyImmature = watchImmatureBalance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(showWatchOnlyImmature); // show watch-only immature balance
}

void OverviewPage::updateCoins(const std::vector<CMintMeta>& spendable, const std::vector<CMintMeta>& pending)
{
    CAmount sum(0);
    int64_t denom;
    for (const auto& c : spendable) {
        DenominationToInteger(c.denom, denom);
        sum += denom;
    }

    CAmount pendingSum(0);
    for (const auto& c : pending) {
        DenominationToInteger(c.denom, denom);
        pendingSum += denom;
    }



    currentSigmaBalance = sum;
    currentSigmaUnconfirmedBalance = pendingSum;
    setSigmaBalance();
}

void OverviewPage::setSigmaBalance()
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();

    ui->labelSigmaBalance->setText(BitcoinUnits::formatWithUnit(unit, currentSigmaBalance, false, BitcoinUnits::separatorAlways));
    ui->labelSigmaPending->setText(BitcoinUnits::formatWithUnit(unit, currentSigmaUnconfirmedBalance, false, BitcoinUnits::separatorAlways));
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly)
        ui->labelWatchImmature->hide();
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(),model->getStake(),
                   model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance());
        connect(model, SIGNAL(balanceChanged(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)), this, SLOT(setBalance(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)));
        connect(model, SIGNAL(notifySigmaChanged(const std::vector<CMintMeta>, const std::vector<CMintMeta>)),
        this, SLOT(updateCoins(const std::vector<CMintMeta>, const std::vector<CMintMeta>)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        updateWatchOnlyLabels(model->haveWatchOnly());
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, currentUnconfirmedBalance, currentImmatureBalance,currentStake,
                       currentWatchOnlyBalance, currentWatchUnconfBalance, currentWatchImmatureBalance);
        setSigmaBalance();

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}
