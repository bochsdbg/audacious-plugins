/*
 * search_bar.cc
 * Copyright 2016 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "search_bar.h"
#include "playlist.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

#include <libaudcore/i18n.h>

SearchBar::SearchBar (QWidget * parent, PlaylistWidget * playlistWidget) :
    QWidget (parent),
    m_playlistWidget (playlistWidget),
    m_entry (new QLineEdit (this))
{
    m_entry->setClearButtonEnabled (true);

    auto layout = new QHBoxLayout (this);
    layout->setContentsMargins (0, 0, 0, 0);
    layout->setSpacing (4);

    layout->addWidget (new QLabel (_("Find:"), this));
    layout->addWidget (m_entry);

    setFocusProxy (m_entry);

    connect (m_entry, & QLineEdit::textChanged, [this] (const QString & text) {
        m_playlistWidget->setFilter (text);
    });
}

void SearchBar::keyPressEvent (QKeyEvent * event)
{
    if (event->modifiers () == Qt::NoModifier)
    {
        switch (event->key ())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            m_playlistWidget->playCurrentIndex ();
            return;

        case Qt::Key_Up:
            m_playlistWidget->moveFocus (-1);
            return;

        case Qt::Key_Down:
            m_playlistWidget->moveFocus (1);
            return;

        case Qt::Key_Escape:
            m_entry->clear ();
            m_playlistWidget->setFocus ();
            hide ();
            return;
        }
    }

    QWidget::keyPressEvent (event);
}