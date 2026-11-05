// FUSE Remaster W1.1 synthetic script (CC0, authored for the test suite): one datablock, its callbacks and a
// scheduled tick, the patterns the W1.6 parser and the W1.7 parity harness look for.

datablock StaticShapeData(FixtureCrateData)
{
   category = "Fixtures";
   shapeFile = "shapes/crate.dts";
};

datablock StaticShapeData(FixtureArmData : FixtureCrateData)
{
   shapeFile = "shapes/arm.dts";
};

function FixtureArmData::onAdd(%this, %obj)
{
   %obj.playThread(0, "wave");
}

function FixtureCrateData::onCollision(%this, %obj, %col)
{
   echo("crate touched by " @ %col.getName());
}

function onFixtureLevelLoaded()
{
   echo("fixture level loaded");
   $Fixture::ticks = 0;
   schedule(1000, 0, "onFixtureTick");
}

function onFixtureTick()
{
   $Fixture::ticks++;
   if ($Fixture::ticks < 3)
      schedule(1000, 0, "onFixtureTick");
}
