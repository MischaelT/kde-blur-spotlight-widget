/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tinyblur.h"

#include <KPluginFactory>

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(TinyBlurEffect,
                                      "metadata.json",
                                      return TinyBlurEffect::supported();
                                      ,
                                      return TinyBlurEffect::enabledByDefault();)

} // namespace KWin

#include "main.moc"
