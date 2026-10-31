#pragma once

#include <QApplication>

namespace pbencoder::test
{

QApplication& EnsureApplication();
void DrainEvents();

}
