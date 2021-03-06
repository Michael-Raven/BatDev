//=======================================================================================
//    MidLevel.ino -- Repository for routines that perform a significant amount
//                    of work in connecting the command handlers to the low-level
//                    drivers.
//=======================================================================================


exitStatus BailOutQ (float busV, float battemp)
{
    exitStatus bailRC = Success;       // success means no bail-outs 'tripped'

    if (battemp > maxBatTemp)   bailRC = MaxTemp;
    if (busV > maxBatVolt)      bailRC = MaxV;
    if (!PowerGoodQ())          bailRC = PBad;
    if (StatusQ() == 1)         bailRC = DiodeTrip;     // will trip if used during off pulse
    if (Serial.available() > 0) bailRC = ConsoleInterrupt;
    if (bailRC != Success)
        Printf("Bail! %1.3f V, %1.2f deg\n", busV, battemp);

    return bailRC;

}


exitStatus BatteryPresentQ (float busV)
{


    exitStatus DetectRC = Success;             // means nimh of correct polarity is found
    float shuntMA;
    
    LoadBus();                                 // slight load removes ~1.3V stray from '219 on empty bus

    if (busV > 1.35) {
        DetectRC = Alky;                       // 1.35 < alky > 1.60
    }
    if (busV > 1.60) {                         // 1.60 < lithium AA > 2.0
        DetectRC = Lithi;
    }
    if (busV > 2.0) {                          // > 2.0, wtf?bp
        DetectRC = UnkBatt;
    }
    if (busV < 0.1) {
        LightOn();                              // a load which passes thru the '219 shunt
        Monitor(&shuntMA, NULL);
        if (shuntMA < 0) {                      // reverse current comes from reverse polarity battery
            DetectRC = BatRev;
        }
        LightOff();
    }    
    UnLoadBus();
    if (digitalRead(BatDetect) == HIGH) { 
        DetectRC = NoBatt;
    }
    return DetectRC;                        // if batt detect pin is low, batt is present
                                            // ..also, 0.0 < busV < 1.35, assume nimh
}



//---------------------------------------------------------------------------------------
//    ConstantVoltage -- Charge at a steady voltage until the given time limit is
//                       exceeded, or any of various charging anomalies occurs.
//---------------------------------------------------------------------------------------

exitStatus ConstantVoltage (float targetV, unsigned int minutes, float mAmpFloor)
{
    float closeEnough = 0.01;
    float shuntMA, busV, ambientTemp, batteryTemp;
    unsigned long timeStamp, lastTime = 0;
    unsigned long endTime = millis() + (minutes * 60 * 1000UL);

    SetVoltage(targetV - 0.050);
    while ((timeStamp = millis()) < endTime) {
        Monitor(&shuntMA, &busV);
        GetTemperatures(&batteryTemp, &ambientTemp);

        if (batteryTemp > 44.9)            return MaxTemp;
        if (busV > MaxV)             return MaxV;
        if (!PowerGoodQ())           return PBad;
        if (Serial.available() > 0)  return ConsoleInterrupt;
        if (shuntMA > mAmpCeiling)   return MaxAmp;
        if (shuntMA < mAmpFloor)     return MinAmp;

        while (busV < (targetV - closeEnough)) {
            if (NudgeVoltage(+1) == 0)
                return BoundsCheck;
            Monitor(&shuntMA, &busV);
        }

        while (busV > (targetV + closeEnough)) {
            if (NudgeVoltage(-1) == 0)
                return BoundsCheck;
            Monitor(&shuntMA, &busV);
        }

        if ((timeStamp - lastTime) > reportInterval) {
            CTReport(typeCVRecord, shuntMA, busV, batteryTemp, ambientTemp, timeStamp);
            lastTime = timeStamp;
        }

    }
    return MaxTime;
}

//---------------------------------------------------------------------------------------
//    ThermMonitor -- Observe thermometer data (plus current & voltage)
//---------------------------------------------------------------------------------------

exitStatus ThermMonitor (int minutes)
{
    float shuntMA, busV, ambientTemp, batteryTemp;

    ResyncTimer(ReportTimer);
    StartTimer(MaxChargeTimer, (minutes * 60 * 10UL));

    while (IsRunning(MaxChargeTimer)) {
        if (Serial.available() > 0)
            return ConsoleInterrupt;

        if (HasExpired(ReportTimer)) {
            Monitor(&shuntMA, &busV);
            GetTemperatures(&batteryTemp, &ambientTemp);
            CTReport(typeThermRecord, shuntMA, busV, batteryTemp, ambientTemp, millis());
        }
    }
    return MaxTime;
}


//---------------------------------------------------------------------------------------
//    Discharge
//---------------------------------------------------------------------------------------

exitStatus Discharge (float thresh1, float thresh2, unsigned reboundTime)
{
    float shuntMA, busV;
    unsigned long endTime, currentTime, reportTime = 0;

    PowerOff();    // Ensure TLynx power isn't just running down the drain.

    Monitor(&shuntMA, &busV);

    HeavyOn();
    while (busV > thresh1) {
        if (Serial.available() > 0) {
            HeavyOff();
            Printf("{9,-1,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
            return ConsoleInterrupt;
        }
        if ((currentTime = millis()) > reportTime) {
            reportTime = currentTime + reportInterval;
            Printf("{9,0,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
        }
        Monitor(&shuntMA, &busV);
    }
    HeavyOff();

    StartTimer(ReboundTimer, reboundTime * 10UL);
    ResyncTimer(ReportTimer);
    while (IsRunning(ReboundTimer)) {
        if (HasExpired(ReportTimer)) {
            Monitor(&shuntMA, &busV);
            Printf("{9,1,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
        }
    }

    LightOn();
    while (busV > thresh2) {
        if (Serial.available() > 0) {
            LightOff();
            Printf("{9,-1,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
            return ConsoleInterrupt;
        }
        if ((currentTime = millis()) > reportTime) {
            reportTime = currentTime + reportInterval;
            Printf("{9,2,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
        }
        Monitor(&shuntMA, &busV);
    }
    LightOff();

    Printf("{9,9,%1.1f,%1.4f,%lu},\n", shuntMA, busV, millis());
    Printx("Discharge Done");
    return Success;

}


//---------------------------------------------------------------------------------------
//    CoolDown  --
//---------------------------------------------------------------------------------------

exitStatus CoolDown (unsigned durationM)
{
    float shuntMA, busV, batteryTemp, ambientTemp;

    ResyncTimer(ReportTimer);
    StartTimer(MaxChargeTimer, (durationM * 60 * 10UL));

    while (IsRunning(MaxChargeTimer)) {
        if (Serial.available() > 0)
            return ConsoleInterrupt;

        if (HasExpired(ReportTimer)) {
            Monitor(&shuntMA, &busV);
            GetTemperatures(&batteryTemp, &ambientTemp);
            CTReport(typeCCRecord, shuntMA, busV, batteryTemp, ambientTemp, millis());
        }
    }
    return MaxTime;
}
