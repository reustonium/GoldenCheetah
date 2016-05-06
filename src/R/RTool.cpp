/*
 * Copyright (c) 2016 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "RTool.h"
#include "RGraphicsDevice.h"
#include "GcUpgrade.h"

#include "RideCache.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideFileCache.h"
#include "Colors.h"
#include "RideMetric.h"
#include "RideMetadata.h"
#include "PMCData.h"
#include "WPrime.h"

#include "Rinternals.h"
#include "Rversion.h"

// message i/o from to R
#ifndef WIN32
#define R_INTERFACE_PTRS
#include <Rinterface.h>
#endif

RTool::RTool()
{
    // setup the R runtime elements
    failed = false;
    starting = true;

    try {

        // yikes, self referenced during construction (!)
        rtool = this;

        // initialise
        R = new REmbed();

        // failed to load
        if (R->loaded == false) {
            failed=true;
            return;
        }

        // capture all output and input to our methods
#ifndef WIN32
        ptr_R_Suicide = &RTool::R_Suicide;
        ptr_R_ShowMessage = &RTool::R_ShowMessage;
        ptr_R_ReadConsole = &RTool::R_ReadConsole;
        ptr_R_WriteConsole = &RTool::R_WriteConsole;
        ptr_R_WriteConsoleEx = &RTool::R_WriteConsoleEx;
        ptr_R_ResetConsole = &RTool::R_ResetConsole;
        ptr_R_FlushConsole = &RTool::R_FlushConsole;
        ptr_R_ClearerrConsole = &RTool::R_ClearerrConsole;
        ptr_R_Busy = &RTool::R_Busy;

        // turn off stderr io
        R_Outputfile = NULL;
        R_Consolefile = NULL;
#endif

        dev = new RGraphicsDevice();

        // register our functions
        R_CMethodDef cMethods[] = {
            { "GC.display", (DL_FUNC) &RGraphicsDevice::GCdisplay, 0 ,0, 0 },
            { "GC.athlete", (DL_FUNC) &RTool::athlete, 0 ,0, 0 },
            { "GC.athlete.home", (DL_FUNC) &RTool::athleteHome, 0 ,0, 0 },
            { "GC.activities", (DL_FUNC) &RTool::activities, 0 ,0, 0 },
            { "GC.activity", (DL_FUNC) &RTool::activity, 0 ,0, 0 },
            { "GC.activity.meanmax", (DL_FUNC) &RTool::activityMeanmax, 0 ,0, 0 },
            { "GC.activity.wbal", (DL_FUNC) &RTool::activityWBal, 0 ,0, 0 },
            { "GC.metrics", (DL_FUNC) &RTool::metrics, 0 ,0, 0 },
            { "GC.pmc", (DL_FUNC) &RTool::pmc, 0 ,0, 0 },
            { NULL, NULL, 0, 0, 0 }
        };
        R_CallMethodDef callMethods[] = {
            { "GC.display", (DL_FUNC) &RGraphicsDevice::GCdisplay, 0 },
            { "GC.athlete", (DL_FUNC) &RTool::athlete, 0 },
            { "GC.athlete.home", (DL_FUNC) &RTool::athleteHome, 0 },
            { "GC.activities", (DL_FUNC) &RTool::activities, 0 },

            // if activity is passed compare=TRUE it returns a list of rides
            // currently in the compare pane if compare is enabled or
            // just a 1 item list with the current ride
            { "GC.activity", (DL_FUNC) &RTool::activity, 1 },
            { "GC.activity.meanmax", (DL_FUNC) &RTool::activityMeanmax, 1 },
            { "GC.activity.wbal", (DL_FUNC) &RTool::activityWBal, 1 },

            // metrics is passed a Rboolean for "all":
            // TRUE -> return all metrics, FALSE -> apply date range selection
            // and a Rboolean for "compare"
            // TRUE -> return a list of compares, FALSE -> return metrics for current date range
            { "GC.metrics", (DL_FUNC) &RTool::metrics, 2 },

            // return a data.frame of pmc series (all=FALSE, metric="TSS")
            { "GC.pmc", (DL_FUNC) &RTool::pmc, 2 },
            { NULL, NULL, 0 }
        };

        // set them up
        DllInfo *info = R_getEmbeddingDllInfo();
        R_registerRoutines(info, cMethods, callMethods, NULL, NULL);

        // lets get the version early for the about dialog
        version = QString("%1.%2").arg(R_MAJOR).arg(R_MINOR);

        // load the dynamix library and create function wrapper
        // we should put this into a source file (.R)
        R->parseEvalNT(QString("options(\"repos\"=\"%3\")\n"
                               "GC.display <- function() { .Call(\"GC.display\") }\n"
                               "GC.athlete <- function() { .Call(\"GC.athlete\") }\n"
                               "GC.athlete.home <- function() { .Call(\"GC.athlete.home\") }\n"
                               "GC.activities <- function() { .Call(\"GC.activities\") }\n"
                               "GC.activity <- function(compare=FALSE) { .Call(\"GC.activity\", compare) }\n"
                               "GC.activity.meanmax <- function(compare=FALSE) { .Call(\"GC.activity.meanmax\", compare) }\n"
                               "GC.activity.wbal <- function(compare=FALSE) { .Call(\"GC.activity.wbal\", compare) }\n"
                               "GC.metrics <- function(all=FALSE, compare=FALSE) { .Call(\"GC.metrics\", all, compare) }\n"
                               "GC.pmc <- function(all=FALSE, metric=\"TSS\") { .Call(\"GC.pmc\", all, metric) }\n"
                               "GC.version <- function() { return(\"%1\") }\n"
                               "GC.build <- function() { return(%2) }\n")
                       .arg(VERSION_STRING)
                       .arg(VERSION_LATEST)
                       .arg("https://cloud.r-project.org/"));

        rtool->messages.clear();

        // set the "GC" object and methods
        context = NULL;
        canvas = NULL;

        // TBD
        // GC.seasons     - configured seasons
        // GC.config      - configuration (zones, units etc)

        // the following are already set in RChart on a per call basis
        // "GC.athlete" "GC.athlete.home"

        configChanged();

    } catch(std::exception& ex) {

        qDebug()<<"Parse error:"  << ex.what();
        failed = true;

    } catch(...) {

        failed = true;
    }

    // ack, disable R runtime
    if (failed) {
        qDebug() << "R Embed failed to start, RConsole disabled.";
        version = "none";
        R = NULL;
    }
    starting = false;
}

void
RTool::configChanged()
{
    // update global R appearances
    QString parCommand=QString("par(bg=\"%1\", "
                               "    col=\"%2\", "
                               "    fg=\"%2\", "
                               "    col.main=\"%2\", "
                               "    col.sub=\"%3\", "
                               "    col.lab=\"%3\", "
                               "    col.axis=\"%3\")"
                            ).arg(GColor(CPLOTBACKGROUND).name())
                             .arg(GCColor::invertColor(GColor(CPLOTBACKGROUND)).name())
                             .arg(GColor(CPLOTMARKER).name());

    // fire and forget, don't care if it fails or not !!
    rtool->R->parseEvalNT(parCommand);
}

SEXP
RTool::athleteHome()
{
    QString returning = ".";
    if (rtool->context) returning = rtool->context->athlete->home->root().absolutePath();

    // convert to R type and return, yuck.
    SEXP ans;
    PROTECT(ans=Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(ans, 0, Rf_mkChar(returning.toLatin1().constData()));
    UNPROTECT(1);
    return ans;
}

SEXP
RTool::athlete()
{
    QString returning = "none";
    if (rtool->context) returning = rtool->context->athlete->cyclist;

    // convert to R type and return, yuck.
    SEXP ans;
    PROTECT(ans=Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(ans, 0, Rf_mkChar(returning.toLatin1().constData()));
    UNPROTECT(1);
    return ans;
}



SEXP
RTool::activities()
{
    SEXP dates=NULL;
    SEXP clas;

    if (rtool->context && rtool->context->athlete && rtool->context->athlete->rideCache) {

        // allocate space for a vector of dates
        PROTECT(dates=Rf_allocVector(REALSXP, rtool->context->athlete->rideCache->count()));

        // fill with values for date and class
        int i=0;
        foreach(RideItem *item, rtool->context->athlete->rideCache->rides()) {
            REAL(dates)[i++] = item->dateTime.toUTC().toTime_t();

        }

        // POSIXct class
        PROTECT(clas=Rf_allocVector(STRSXP, 2));
        SET_STRING_ELT(clas, 0, Rf_mkChar("POSIXct"));
        SET_STRING_ELT(clas, 1, Rf_mkChar("POSIXt"));
        Rf_classgets(dates,clas);

        // we use "UTC" for all timezone
        Rf_setAttrib(dates, Rf_install("tzone"), Rf_mkString("UTC"));

        UNPROTECT(2);
    }

    return dates;
}

SEXP
RTool::dfForDateRange(bool all, DateRange range)
{
    const RideMetricFactory &factory = RideMetricFactory::instance();
    int rides = rtool->context->athlete->rideCache->count();
    int metrics = factory.metricCount();

    // count the number of meta fields to add
    int meta = 0;
    if (rtool->context && rtool->context->athlete->rideMetadata()) {

        // count active fields
        foreach(FieldDefinition def, rtool->context->athlete->rideMetadata()->getFields()) {
            if (def.name != "" && def.tab != "" && !rtool->context->specialFields.isSpecial(def.name) &&
                !rtool->context->specialFields.isMetric(def.name))
                meta++;
        }
    }

    // how many rides to return if we're limiting to the
    // currently selected date range ?
    if (!all) {
        // we need to count rides that are in range...
        rides = 0;
        foreach(RideItem *ride, rtool->context->athlete->rideCache->rides()) {
            if (range.pass(ride->dateTime.date())) rides++;
        }
    }

    // get a listAllocated
    SEXP ans;
    SEXP names; // column names
    SEXP rownames; // row names (numeric)

    // +3 is for date and datetime and color
    PROTECT(ans=Rf_allocList(metrics+meta+3));
    PROTECT(names = Rf_allocVector(STRSXP, metrics+meta+3));

    // we have to give a name to each row
    PROTECT(rownames = Rf_allocVector(STRSXP, rides));
    for(int i=0; i<rides; i++) {
        QString rownumber=QString("%1").arg(i+1);
        SET_STRING_ELT(rownames, i, Rf_mkChar(rownumber.toLatin1().constData()));
    }

    // next name, nextS is next metric
    int next=0;
    SEXP nextS = ans;

    // DATE
    SEXP date;
    PROTECT(date=Rf_allocVector(INTSXP, rides));

    int k=0;
    QDate d1970(1970,01,01);
    foreach(RideItem *ride, rtool->context->athlete->rideCache->rides()) {
        if (all || range.pass(ride->dateTime.date()))
            INTEGER(date)[k++] = d1970.daysTo(ride->dateTime.date());
    }

    SEXP dclas;
    PROTECT(dclas=Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(dclas, 0, Rf_mkChar("Date"));
    Rf_classgets(date,dclas);

    // add to the data.frame and give it a name
    SETCAR(nextS, date); nextS=CDR(nextS);
    SET_STRING_ELT(names, next++, Rf_mkChar("date"));

    // TIME
    SEXP time;
    PROTECT(time=Rf_allocVector(REALSXP, rides));

    // fill with values for date and class if its one we need to return
    k=0;
    foreach(RideItem *ride, rtool->context->athlete->rideCache->rides()) {
        if (all || range.pass(ride->dateTime.date()))
            REAL(time)[k++] = ride->dateTime.toUTC().toTime_t();
    }

    // POSIXct class
    SEXP clas;
    PROTECT(clas=Rf_allocVector(STRSXP, 2));
    SET_STRING_ELT(clas, 0, Rf_mkChar("POSIXct"));
    SET_STRING_ELT(clas, 1, Rf_mkChar("POSIXt"));
    Rf_classgets(time,clas);

    // we use "UTC" for all timezone
    Rf_setAttrib(time, Rf_install("tzone"), Rf_mkString("UTC"));

    // add to the data.frame and give it a name
    SETCAR(nextS, time); nextS=CDR(nextS);
    SET_STRING_ELT(names, next++, Rf_mkChar("time"));

    // time + clas, but not ans!
    UNPROTECT(4);

    //
    // METRICS
    //
    for(int i=0; i<factory.metricCount();i++) {

        // set a vector
        SEXP m;
        PROTECT(m=Rf_allocVector(REALSXP, rides));

        QString symbol = factory.metricName(i);
        const RideMetric *metric = factory.rideMetric(symbol);
        QString name = rtool->context->specialFields.internalName(factory.rideMetric(symbol)->name());
        name = name.replace(" ","_");
        name = name.replace("'","_");

        bool useMetricUnits = rtool->context->athlete->useMetricUnits;

        int index=0;
        foreach(RideItem *item, rtool->context->athlete->rideCache->rides()) {
            if (all || range.pass(item->dateTime.date())) {
                REAL(m)[index++] = item->metrics()[i] * (useMetricUnits ? 1.0f : metric->conversion())
                                                      + (useMetricUnits ? 0.0f : metric->conversionSum());
            }
        }

        // add to the list
        SETCAR(nextS, m);
        nextS = CDR(nextS);

        // give it a name
        SET_STRING_ELT(names, next, Rf_mkChar(name.toLatin1().constData()));

        next++;

        // vector
        UNPROTECT(1);
    }

    //
    // META
    //
    foreach(FieldDefinition field, rtool->context->athlete->rideMetadata()->getFields()) {

        // don't add incomplete meta definitions or metric override fields
        if (field.name == "" || field.tab == "" || rtool->context->specialFields.isSpecial(field.name) ||
            rtool->context->specialFields.isMetric(field.name)) continue;

        // Create a string vector
        SEXP m;
        PROTECT(m=Rf_allocVector(STRSXP, rides));

        int index=0;
        foreach(RideItem *item, rtool->context->athlete->rideCache->rides()) {
            if (all || range.pass(item->dateTime.date())) {
                SET_STRING_ELT(m, index++, Rf_mkChar(item->getText(field.name, "").toLatin1().constData()));
            }
        }

        // add to the list
        SETCAR(nextS, m);
        nextS = CDR(nextS);

        // give it a name
        SET_STRING_ELT(names, next, Rf_mkChar(field.name.replace(" ","_").toLatin1().constData()));

        next++;

        // vector
        UNPROTECT(1);
    }

    // add Color
    SEXP color;
    PROTECT(color=Rf_allocVector(STRSXP, rides));

    int index=0;
    foreach(RideItem *item, rtool->context->athlete->rideCache->rides()) {
        if (all || range.pass(item->dateTime.date())) {

            // apply item color, remembering that 1,1,1 means use default (reverse in this case)
            if (item->color == QColor(1,1,1,1)) {

                // use the inverted color, not plot marker as that hideous
                QColor col =GCColor::invertColor(GColor(CPLOTBACKGROUND));

                // white is jarring on a dark background!
                if (col==QColor(Qt::white)) col=QColor(127,127,127);

                SET_STRING_ELT(color, index++, Rf_mkChar(col.name().toLatin1().constData()));
            } else
                SET_STRING_ELT(color, index++, Rf_mkChar(item->color.name().toLatin1().constData()));
        }
    }

    // add to the list and name it
    SETCAR(nextS, color);
    nextS = CDR(nextS);
    SET_STRING_ELT(names, next, Rf_mkChar("color"));
    next++;

    UNPROTECT(1);

    // turn the list into a data frame + set column names
    Rf_setAttrib(ans, R_ClassSymbol, Rf_mkString("data.frame"));
    Rf_setAttrib(ans, R_RowNamesSymbol, rownames);
    Rf_namesgets(ans, names);

    // ans + names
    UNPROTECT(3);

    // return it
    return ans;
}

SEXP
RTool::metrics(SEXP pAll, SEXP pCompare)
{
    // p1 - all=TRUE|FALSE - return all metrics or just within
    //                       the currently selected date range
    pAll = Rf_coerceVector(pAll, LGLSXP);
    bool all = LOGICAL(pAll)[0];

    // p2 - all=TRUE|FALSE - return list of compares (or current if not active)
    pCompare = Rf_coerceVector(pCompare, LGLSXP);
    bool compare = LOGICAL(pCompare)[0];

    // want a list of compares not a dataframe
    if (compare && rtool->context) {

        // only return compares if its actually active
        if (rtool->context->isCompareDateRanges) {

            // how many to return?
            int count=0;
            foreach(CompareDateRange p, rtool->context->compareDateRanges) if (p.isChecked()) count++;

            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, count));

            // start at the front
            SEXP nextS = list;

            // a named list with data.frame 'metrics' and color 'color'
            SEXP namedlist;

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("metrics"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // create a data.frame for each and add to list
            foreach(CompareDateRange p, rtool->context->compareDateRanges) {
                if (p.isChecked()) {

                    // create a named list
                    PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
                    SEXP offset = namedlist;

                    // add the ride
                    SEXP df = rtool->dfForDateRange(all, DateRange(p.start, p.end));
                    SETCAR(offset, df);
                    offset=CDR(offset);

                    // add the color
                    SEXP color;
                    PROTECT(color=Rf_allocVector(STRSXP, 1));
                    SET_STRING_ELT(color, 0, Rf_mkChar(p.color.name().toLatin1().constData()));
                    SETCAR(offset, color);

                    // name them
                    Rf_namesgets(namedlist, names);

                    // add to back and move on
                    SETCAR(nextS, namedlist);
                    nextS=CDR(nextS);

                    UNPROTECT(2);
                }
            }
            UNPROTECT(2); // list and names

            return list;

        } else { // compare isn't active...

            // otherwise return the current metrics in a compare list
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, 1));

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("metrics"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // named list of metrics and color
            SEXP namedlist;
            PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
            SEXP offset = namedlist;

            // add the metrics
            DateRange range = rtool->context->currentDateRange();
            SEXP df = rtool->dfForDateRange(all, range);
            SETCAR(offset, df);
            offset=CDR(offset);

            // add the color
            SEXP color;
            PROTECT(color=Rf_allocVector(STRSXP, 1));
            SET_STRING_ELT(color, 0, Rf_mkChar("#FF00FF"));
            SETCAR(offset, color);

            // name them
            Rf_namesgets(namedlist, names);

            // add to back and move on
            SETCAR(list, namedlist);
            UNPROTECT(4);

            return list;
        }

    } else if (rtool->context && rtool->context->athlete && rtool->context->athlete->rideCache) {

        // just a datafram of metrics
        DateRange range = rtool->context->currentDateRange();
        return rtool->dfForDateRange(all, range);

    }

    // fail
    return Rf_allocVector(INTSXP, 0);
}

SEXP
RTool::dfForActivity(RideFile *f)
{
    // return a data frame for the ride passed
    SEXP ans;

    int points = f->dataPoints().count();

    // how many series?
    int seriescount=0;
    for(int i=0; i<static_cast<int>(RideFile::none); i++) {
        RideFile::SeriesType series = static_cast<RideFile::SeriesType>(i);
        if (i > 15 && !f->isDataPresent(series)) continue;
        seriescount++;
    }

    // if we have any series we will continue and add a 'time' series
    if (seriescount) seriescount++;
    else return Rf_allocVector(INTSXP, 0);

    // we return a list of series vectors
    PROTECT(ans = Rf_allocList(seriescount));

    // we collect the names as we go
    SEXP names;
    PROTECT(names = Rf_allocVector(STRSXP, seriescount)); // names attribute (column names)
    int next=0;
    SEXP nextS = ans;

    //
    // Now we need to add vectors to the ans list...
    //

    // TIME

    // add in actual time in POSIXct format
    SEXP time;
    PROTECT(time=Rf_allocVector(REALSXP, points));

    // fill with values for date and class
    for(int k=0; k<points; k++) REAL(time)[k] = f->startTime().addSecs(f->dataPoints()[k]->secs).toUTC().toTime_t();

    // POSIXct class
    SEXP clas;
    PROTECT(clas=Rf_allocVector(STRSXP, 2));
    SET_STRING_ELT(clas, 0, Rf_mkChar("POSIXct"));
    SET_STRING_ELT(clas, 1, Rf_mkChar("POSIXt"));
    Rf_classgets(time,clas);

    // we use "UTC" for all timezone
    Rf_setAttrib(time, Rf_install("tzone"), Rf_mkString("UTC"));

    // add to the data.frame and give it a name
    SETCAR(nextS, time); nextS=CDR(nextS);
    SET_STRING_ELT(names, next++, Rf_mkChar("time"));

    // time + clas, but not ans!
    UNPROTECT(2);

    // add to the end

    // PRESENT SERIES
    for(int i=0; i < static_cast<int>(RideFile::none); i++) {

        // what series we working with?
        RideFile::SeriesType series = static_cast<RideFile::SeriesType>(i);

        // lets not add lots of NA for the more obscure data series
        if (i > 15 && !f->isDataPresent(series)) continue;

        // set a vector
        SEXP vector;
        PROTECT(vector=Rf_allocVector(REALSXP, points));

        for(int j=0; j<points; j++) {
            if (f->isDataPresent(series)) {
                if (f->dataPoints()[j]->value(series) == 0 && (series == RideFile::lat || series == RideFile::lon))
                    REAL(vector)[j] = NA_REAL;
                else
                    REAL(vector)[j] = f->dataPoints()[j]->value(series);
            } else {
                REAL(vector)[j] = NA_REAL;
            }
        }

        // add to the list
        SETCAR(nextS, vector);
        nextS = CDR(nextS);

        // give it a name
        SET_STRING_ELT(names, next, Rf_mkChar(f->seriesName(series, true).toLatin1().constData()));

        next++;

        // vector
        UNPROTECT(1);
    }

    // add rownames
    SEXP rownames;
    PROTECT(rownames = Rf_allocVector(STRSXP, points));
    for(int i=0; i<points; i++) {
        QString rownumber=QString("%1").arg(i+1);
        SET_STRING_ELT(rownames, i, Rf_mkChar(rownumber.toLatin1().constData()));
    }

    // turn the list into a data frame + set column names
    Rf_setAttrib(ans, R_RowNamesSymbol, rownames);
    Rf_setAttrib(ans, R_ClassSymbol, Rf_mkString("data.frame"));
    Rf_namesgets(ans, names);

    // ans + names + rownames
    UNPROTECT(3);

    // return a valid result
    return ans;
}

SEXP
RTool::activity(SEXP pCompare)
{
    // a dataframe to return
    SEXP ans=NULL;

    // p1 - compare=TRUE|FALSE - return list of compare rides if active, or just current
    pCompare = Rf_coerceVector(pCompare, LGLSXP);
    bool compare = LOGICAL(pCompare)[0];

    // return a list
    if (compare && rtool->context) {


        if (rtool->context->isCompareIntervals) {

            // how many to return?
            int count=0;
            foreach(CompareInterval p, rtool->context->compareIntervals) if (p.isChecked()) count++;

            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, count));

            // start at the front
            SEXP nextS = list;

            // a named list with data.frame 'activity' and color 'color'
            SEXP namedlist;

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("activity"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // create a data.frame for each and add to list
            foreach(CompareInterval p, rtool->context->compareIntervals) {
                if (p.isChecked()) {

                    // create a named list
                    PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
                    SEXP offset = namedlist;

                    // add the ride
                    SEXP df = rtool->dfForActivity(p.rideItem->ride());
                    SETCAR(offset, df);
                    offset=CDR(offset);

                    // add the color
                    SEXP color;
                    PROTECT(color=Rf_allocVector(STRSXP, 1));
                    SET_STRING_ELT(color, 0, Rf_mkChar(p.color.name().toLatin1().constData()));
                    SETCAR(offset, color);

                    // name them
                    Rf_namesgets(namedlist, names);

                    // add to back and move on
                    SETCAR(nextS, namedlist);
                    nextS=CDR(nextS);

                    UNPROTECT(2);
                }
            }
            UNPROTECT(2); // list and names

            return list;

        } else if(rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // just return a list of one ride
            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, 1));

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("activity"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // named list of activity and color
            SEXP namedlist;
            PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
            SEXP offset = namedlist;

            // add the ride
            RideFile *f = const_cast<RideItem*>(rtool->context->currentRideItem())->ride();
            f->recalculateDerivedSeries();
            SEXP df = rtool->dfForActivity(f);
            SETCAR(offset, df);
            offset=CDR(offset);

            // add the color
            SEXP color;
            PROTECT(color=Rf_allocVector(STRSXP, 1));
            SET_STRING_ELT(color, 0, Rf_mkChar("#FF00FF"));
            SETCAR(offset, color);

            // name them
            Rf_namesgets(namedlist, names);

            // add to back and move on
            SETCAR(list, namedlist);
            UNPROTECT(4);

            return list;
        }

    } else if (!compare) { // not compare, so just return a dataframe

        // access via global as this is a static function
        if(rtool->context && rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // get the ride
            RideFile *f = const_cast<RideItem*>(rtool->context->currentRideItem())->ride();
            f->recalculateDerivedSeries();

            // get as a data frame
            ans = rtool->dfForActivity(f);
            return ans;
        }
    }

    // nothing to return
    return Rf_allocVector(INTSXP, 0);
}

SEXP
RTool::dfForActivityMeanmax(const RideItem *i)
{
    // how many series and how big are they?
    unsigned int seriescount=0, size=0;

    // get the meanmax array
    RideFileCache *cache = const_cast<RideItem*>(i)->fileCache();
    if (cache != NULL) {
        // how many points in the ridefilecache and how many series to return
        foreach(RideFile::SeriesType series, cache->meanMaxList()) {
            QVector <double> values = cache->meanMaxArray(series);
            if (values.count()) {
                size = values.count();
                seriescount++;
            }
        }
    }

    // we return a list of series vectors
    SEXP ans;
    PROTECT(ans = Rf_allocList(seriescount));

    // we collect the names as we go
    SEXP names;
    PROTECT(names = Rf_allocVector(STRSXP, seriescount)); // names attribute (column names)
    int next=0;
    SEXP nextS = ans;

    //
    // Now we need to add vectors to the ans list...
    //

    foreach(RideFile::SeriesType series, cache->meanMaxList()) {

        QVector <double> values = cache->meanMaxArray(series);

        // don't add empty ones
        if (values.count()==0) continue;


        // set a vector
        SEXP vector;
        PROTECT(vector=Rf_allocVector(REALSXP, size));

        for(unsigned int j=0; j<size; j++) REAL(vector)[j] = values[j];

        // add to the list
        SETCAR(nextS, vector);
        nextS = CDR(nextS);

        // give it a name
        SET_STRING_ELT(names, next, Rf_mkChar(RideFile::seriesName(series, true).toLatin1().constData()));

        next++;

        // vector
        UNPROTECT(1);
    }

    // add rownames
    SEXP rownames;
    PROTECT(rownames = Rf_allocVector(STRSXP, size));
    for(unsigned int i=0; i<size; i++) {
        QString rownumber=QString("%1").arg(i+1);
        SET_STRING_ELT(rownames, i, Rf_mkChar(rownumber.toLatin1().constData()));
    }

    // turn the list into a data frame + set column names
    Rf_setAttrib(ans, R_RowNamesSymbol, rownames);
    Rf_setAttrib(ans, R_ClassSymbol, Rf_mkString("data.frame"));
    Rf_namesgets(ans, names);

    // ans + names + rownames
    UNPROTECT(3);

    // return a valid result
    return ans;
}

SEXP
RTool::activityMeanmax(SEXP pCompare)
{
    // a dataframe to return
    SEXP ans=NULL;

    // p1 - compare=TRUE|FALSE - return list of compare rides if active, or just current
    pCompare = Rf_coerceVector(pCompare, LGLSXP);
    bool compare = LOGICAL(pCompare)[0];

    // return a list
    if (compare && rtool->context) {


        if (rtool->context->isCompareIntervals) {

            // how many to return?
            int count=0;
            foreach(CompareInterval p, rtool->context->compareIntervals) if (p.isChecked()) count++;

            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, count));

            // start at the front
            SEXP nextS = list;

            // a named list with data.frame 'activity' and color 'color'
            SEXP namedlist;

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("meanmax"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // create a data.frame for each and add to list
            foreach(CompareInterval p, rtool->context->compareIntervals) {
                if (p.isChecked()) {

                    // create a named list
                    PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
                    SEXP offset = namedlist;

                    // add the ride
                    SEXP df = rtool->dfForActivityMeanmax(p.rideItem);
                    SETCAR(offset, df);
                    offset=CDR(offset);

                    // add the color
                    SEXP color;
                    PROTECT(color=Rf_allocVector(STRSXP, 1));
                    SET_STRING_ELT(color, 0, Rf_mkChar(p.color.name().toLatin1().constData()));
                    SETCAR(offset, color);

                    // name them
                    Rf_namesgets(namedlist, names);

                    // add to back and move on
                    SETCAR(nextS, namedlist);
                    nextS=CDR(nextS);

                    UNPROTECT(2);
                }
            }
            UNPROTECT(2); // list and names

            return list;

        } else if(rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // just return a list of one ride
            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, 1));

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("meanmax"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // named list of activity and color
            SEXP namedlist;
            PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
            SEXP offset = namedlist;

            // add the ride
            SEXP df = rtool->dfForActivityMeanmax(rtool->context->currentRideItem());
            SETCAR(offset, df);
            offset=CDR(offset);

            // add the color
            SEXP color;
            PROTECT(color=Rf_allocVector(STRSXP, 1));
            SET_STRING_ELT(color, 0, Rf_mkChar("#FF00FF"));
            SETCAR(offset, color);

            // name them
            Rf_namesgets(namedlist, names);

            // add to back and move on
            SETCAR(list, namedlist);
            UNPROTECT(4);

            return list;
        }

    } else if (!compare) { // not compare, so just return a dataframe

        // access via global as this is a static function
        if(rtool->context && rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // get as a data frame
            ans = rtool->dfForActivityMeanmax(rtool->context->currentRideItem());
            return ans;
        }
    }

    // nothing to return
    return Rf_allocVector(INTSXP, 0);
}

SEXP
RTool::pmc(SEXP pAll, SEXP pMetric)
{
    // parse parameters
    // p1 - all=TRUE|FALSE - return all metrics or just within
    //                       the currently selected date range
    pAll = Rf_coerceVector(pAll, LGLSXP);
    bool all = LOGICAL(pAll)[0];

    // get the currently selected date range
    DateRange range(rtool->context->currentDateRange());

    // p2 - all=TRUE|FALSE - return list of compares (or current if not active)
    pMetric = Rf_coerceVector(pMetric, STRSXP);
    QString metric (CHAR(STRING_ELT(pMetric,0)));

    // return a dataframe with PMC data for all or the current season
    // XXX uses the default half-life
    if (rtool->context) {

        // convert the name to a symbol, if not found just leave as it is
        const RideMetricFactory &factory = RideMetricFactory::instance();
        for (int i=0; i<factory.metricCount(); i++) {
            QString symbol = factory.metricName(i);
            QString name = rtool->context->specialFields.internalName(factory.rideMetric(symbol)->name());
            name.replace(" ","_");

            if (name == metric) {
                metric = symbol;
                break;
            }
        }

        // create the data
        PMCData pmcData(rtool->context, Specification(), metric);

        // how many entries ?
        QDate d1970(1970,01,01);

        // not unsigned coz date could be configured < 1970 (!)
        int from =d1970.daysTo(range.from);
        int to =d1970.daysTo(range.to);
        unsigned int size = all ? pmcData.days() : (to - from + 1);

        // returning a dataframe with
        // date, lts, sts, sb, rr
        SEXP ans, names;

        // date, stress, lts, sts, sb, rr
        PROTECT(ans=Rf_allocList(6));
        SEXP nextS = ans;

        // set ther names
        PROTECT(names = Rf_allocVector(STRSXP, 6));
        SET_STRING_ELT(names, 0, Rf_mkChar("date"));
        SET_STRING_ELT(names, 1, Rf_mkChar("stress"));
        SET_STRING_ELT(names, 2, Rf_mkChar("lts"));
        SET_STRING_ELT(names, 3, Rf_mkChar("sts"));
        SET_STRING_ELT(names, 4, Rf_mkChar("sb"));
        SET_STRING_ELT(names, 5, Rf_mkChar("rr"));

        // DATE - 1 a day from start
        SEXP date;
        PROTECT(date=Rf_allocVector(INTSXP, size));
        unsigned int start = d1970.daysTo(all ? pmcData.start() : range.from);
        for(unsigned int k=0; k<size; k++) INTEGER(date)[k] = start + k;

        SEXP dclas;
        PROTECT(dclas=Rf_allocVector(STRSXP, 1));
        SET_STRING_ELT(dclas, 0, Rf_mkChar("Date"));
        Rf_classgets(date,dclas);

        // add to the data.frame
        SETCAR(nextS, date); nextS=CDR(nextS);

        // PMC DATA

        SEXP stress, lts, sts, sb, rr;
        PROTECT(stress=Rf_allocVector(REALSXP, size));
        PROTECT(lts=Rf_allocVector(REALSXP, size));
        PROTECT(sts=Rf_allocVector(REALSXP, size));
        PROTECT(sb=Rf_allocVector(REALSXP, size));
        PROTECT(rr=Rf_allocVector(REALSXP, size));

            int index=0;
        if (all) {

            // just copy
            for(unsigned int k=0; k<size; k++)  REAL(stress)[k] = pmcData.stress()[k];
            for(unsigned int k=0; k<size; k++)  REAL(lts)[k] = pmcData.lts()[k];
            for(unsigned int k=0; k<size; k++)  REAL(sts)[k] = pmcData.sts()[k];
            for(unsigned int k=0; k<size; k++)  REAL(sb)[k] = pmcData.sb()[k];
            for(unsigned int k=0; k<size; k++)  REAL(rr)[k] = pmcData.rr()[k];

        } else {

            int day = d1970.daysTo(pmcData.start());
            for(int k=0; k < pmcData.days(); k++) {

                // day today
                if (day >= from && day <= to) {

                    REAL(stress)[index] = pmcData.stress()[k];
                    REAL(lts)[index] = pmcData.lts()[k];
                    REAL(sts)[index] = pmcData.sts()[k];
                    REAL(sb)[index] = pmcData.sb()[k];
                    REAL(rr)[index] = pmcData.rr()[k];
                    index++;
                }
                day++;
            }
        }

        // add to the list
        SETCAR(nextS, stress); nextS = CDR(nextS);
        SETCAR(nextS, lts); nextS = CDR(nextS);
        SETCAR(nextS, sts); nextS = CDR(nextS);
        SETCAR(nextS, sb); nextS = CDR(nextS);
        SETCAR(nextS, rr); nextS = CDR(nextS);

        SEXP rownames;
        PROTECT(rownames = Rf_allocVector(STRSXP, size));
        for(unsigned int i=0; i<size; i++) {
            QString rownumber=QString("%1").arg(i+1);
            SET_STRING_ELT(rownames, i, Rf_mkChar(rownumber.toLatin1().constData()));
        }

        // turn the list into a data frame + set column names
        Rf_setAttrib(ans, R_ClassSymbol, Rf_mkString("data.frame"));
        Rf_setAttrib(ans, R_RowNamesSymbol, rownames);
        Rf_namesgets(ans, names);

        UNPROTECT(10);

        // return it
        return ans;
    }

    // nothing to return
    return Rf_allocVector(INTSXP, 0);
}

SEXP
RTool::activityWBal(SEXP pCompare)
{
    SEXP ans=NULL;

    // p1 - compare=TRUE|FALSE - return list of compare rides if active, or just current
    pCompare = Rf_coerceVector(pCompare, LGLSXP);
    bool compare = LOGICAL(pCompare)[0];

    // return a list
    if (compare && rtool->context) {


        if (rtool->context->isCompareIntervals) {

            // how many to return?
            int count=0;
            foreach(CompareInterval p, rtool->context->compareIntervals) if (p.isChecked()) count++;

            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, count));

            // start at the front
            SEXP nextS = list;

            // a named list with data.frame 'activity' and color 'color'
            SEXP namedlist;

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("activity"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // create a data.frame for each and add to list
            foreach(CompareInterval p, rtool->context->compareIntervals) {
                if (p.isChecked()) {

                    // create a named list
                    PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
                    SEXP offset = namedlist;

                    // add the ride
                    SEXP df = rtool->dfForActivityWBal(p.rideItem->ride());
                    SETCAR(offset, df);
                    offset=CDR(offset);

                    // add the color
                    SEXP color;
                    PROTECT(color=Rf_allocVector(STRSXP, 1));
                    SET_STRING_ELT(color, 0, Rf_mkChar(p.color.name().toLatin1().constData()));
                    SETCAR(offset, color);

                    // name them
                    Rf_namesgets(namedlist, names);

                    // add to back and move on
                    SETCAR(nextS, namedlist);
                    nextS=CDR(nextS);

                    UNPROTECT(2);
                }
            }
            UNPROTECT(2); // list and names

            return list;

        } else if(rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // just return a list of one ride
            // cool we can return a list of intervals to compare
            SEXP list;
            PROTECT(list=Rf_allocVector(LISTSXP, 1));

            // names
            SEXP names;
            PROTECT(names=Rf_allocVector(STRSXP, 2));
            SET_STRING_ELT(names, 0, Rf_mkChar("activity"));
            SET_STRING_ELT(names, 1, Rf_mkChar("color"));

            // named list of activity and color
            SEXP namedlist;
            PROTECT(namedlist=Rf_allocVector(LISTSXP, 2));
            SEXP offset = namedlist;

            // add the ride
            RideFile *f = const_cast<RideItem*>(rtool->context->currentRideItem())->ride();
            f->recalculateDerivedSeries();
            SEXP df = rtool->dfForActivityWBal(f);
            SETCAR(offset, df);
            offset=CDR(offset);

            // add the color
            SEXP color;
            PROTECT(color=Rf_allocVector(STRSXP, 1));
            SET_STRING_ELT(color, 0, Rf_mkChar("#FF00FF"));
            SETCAR(offset, color);

            // name them
            Rf_namesgets(namedlist, names);

            // add to back and move on
            SETCAR(list, namedlist);
            UNPROTECT(4);

            return list;
        }

    } else if (!compare) { // not compare, so just return a dataframe

        // access via global as this is a static function
        if(rtool->context && rtool->context->currentRideItem() && const_cast<RideItem*>(rtool->context->currentRideItem())->ride()) {

            // get the ride
            RideFile *f = const_cast<RideItem*>(rtool->context->currentRideItem())->ride();
            f->recalculateDerivedSeries();

            // get as a data frame
            ans = rtool->dfForActivityWBal(f);
            return ans;
        }
    }

    // nothing to return
    return Rf_allocVector(INTSXP, 0);
}

SEXP
RTool::dfForActivityWBal(RideFile*f)
{
    // return a data frame with wpbal in 1s samples
    if(f && f->wprimeData()) {

        // get as a data frame
        WPrime *w = f->wprimeData();

        if (w && w->ydata().count() >0) {

                // construct a vector
                SEXP ans;
                PROTECT(ans=Rf_allocVector(REALSXP, w->ydata().count()));

                // add values
                for(int i=0; i<w->ydata().count(); i++) REAL(ans)[i] = w->ydata()[i];

                UNPROTECT(1);

                return ans;
        }
    }

    // nothing to return
    return Rf_allocVector(INTSXP, 0);
}