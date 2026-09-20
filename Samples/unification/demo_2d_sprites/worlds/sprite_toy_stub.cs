module "SpriteToyStub";

new SceneToy() {
  new SpritePlayer(Hero) {
    position = "0 0";
    layer = 1;
    sortPoint = 10;
    physicsEnabled = true;
    collisionRadius = 0.5;
    collisionLayer = 1;
    collisionMask = 0xFFFFFFFF;
    imageMap = "HeroSheet.png";
    animationName = "Walk";
    frameCount = 8;
    animationFPS = 12;
  };
  new SpritePlayer(Companion) {
    position = "2 1";
    layer = 0;
    sortPoint = 5;
    physicsEnabled = false;
  };
  new SpritePlayer(Backdrop) {
    position = "-1 -1";
    layer = -1;
    sortPoint = 1;
  };
};
