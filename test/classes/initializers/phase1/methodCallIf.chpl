class Foo {
  var x: int;

  proc init(xVal) {
    x = if xVal > 5 then option1() else option2();
    super.init();
  }

  proc option1() {
    return 2;
  }

  proc option2() {
    return 3;
  }
}

var foo = new Foo(10);
writeln(foo);
delete foo;
