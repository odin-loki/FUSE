module "AfxSpriteStub";

new SceneToy() {
  new SpritePlayer(FxSocketSprite) {
    position = "0 0";
    layer = 1;
    sortPoint = 15;
    physicsEnabled = false;
    imageMap = "FxSparkSheet.png";
    animationName = "SparkLoop";
    frameCount = 4;
    animationFPS = 15;
  };
  new SpritePlayer(FxAmbientSprite) {
    position = "1.5 0.5";
    layer = 0;
    sortPoint = 5;
  };
};
