/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.arrow.dataset.jni;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicLong;

import org.apache.arrow.memory.util.MemoryUtil;
import org.apache.arrow.util.VisibleForTesting;

/**
 * Reserving Java direct memory bytes from java.nio.Bits. Used by Java Dataset API's C++ memory
 * pool implementation. This makes memory allocated by the pool to be controlled by JVM option
 * "-XX:MaxDirectMemorySize".
 */
public class DirectReservationListener implements ReservationListener {

  private static final DirectReservationListener INSTANCE = new DirectReservationListener();

  public static DirectReservationListener instance() {
    return INSTANCE;
  }

  /**
   * Reserve bytes by invoking java.nio.java.Bitjava.nio.Bitss#reserveMemory.
   */
  @Override
  public void reserve(long size) {
    MemoryUtil.reserveDirectMemory(size);
  }

  /**
   * Unreserve bytes by invoking java.nio.java.Bitjava.nio.Bitss#unreserveMemory.
   */
  @Override
  public void unreserve(long size) {
    MemoryUtil.unreserveDirectMemory(size);
  }

  /**
   * Get current reservation of jVM direct memory. Visible for testing.
   */
  @VisibleForTesting
  public long getCurrentDirectMemReservation() {
    return MemoryUtil.getCurrentDirectMemReservation();
  }
}
