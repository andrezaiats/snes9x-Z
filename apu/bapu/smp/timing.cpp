template<unsigned cycle_frequency>
void SMP::Timer<cycle_frequency>::tick(unsigned clocks) {
  stage1_ticks += clocks;
  if(stage1_ticks < cycle_frequency) return;

  stage1_ticks -= cycle_frequency;
  if(enable == false) return;

  if(++stage2_ticks != target) return;

  stage2_ticks = 0;
  stage3_ticks = (stage3_ticks + 1) & 15;
}





void SMP::timer_step() {
  unsigned elapsed = timer_batch - timer_countdown;
  timer0.tick(elapsed);
  timer1.tick(elapsed);
  timer2.tick(elapsed);
  timer_resync();
}

void SMP::timer_flush() {
  unsigned elapsed = timer_batch - timer_countdown;
  if(elapsed) {
    timer0.tick(elapsed);
    timer1.tick(elapsed);
    timer2.tick(elapsed);
  }
  timer_resync();
}

void SMP::timer_resync() {
  int n0 = timer0.stage1_ticks < 128 ? 128 - timer0.stage1_ticks : 1;
  int n1 = timer1.stage1_ticks < 128 ? 128 - timer1.stage1_ticks : 1;
  int n2 = timer2.stage1_ticks <  16 ?  16 - timer2.stage1_ticks : 1;
  int n = n0 < n1 ? n0 : n1;
  if(n2 < n) n = n2;
  timer_batch = timer_countdown = n;
}
